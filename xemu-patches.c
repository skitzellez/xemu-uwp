/*
 * Copyright (c) 2026 Skitzelles - part of Game Vault's xemu for Xbox Dev Mode.
 *
 * xemu game patches: small changes to an Xbox game's code or data while it runs, for any game.
 *
 * A patch file is a text file named after the game's title ID (8 hex digits, e.g. 54430003.txt), read from
 *   1. the patches folder next to xemu.exe (shipped with xemu), and
 *   2. the patches folder next to xemu's log and settings (LocalState\xemu\xemu\patches; testing, own patches).
 * A file in 2 replaces the file of the same name in 1; with neither, the patches built into xemu
 * (gv-patches folder, which arrive with xemu's own updates) are used.
 *
 *   # comment
 *   title 54430003            (optional: the title ID this file is for; the file name already says it)
 *   version 0x00000001        (optional: only this version of the game, from its certificate)
 *
 *   [ta-difficulty] Time Attack uses your difficulty
 *   default off               (on or off when Game Vault's link does not say; on if missing)
 *   0x0012ABCD: 6A 00 -> 6A 03
 *   0x0012ABF0: 74 05 -> 90 90
 *
 * Each line of a patch: the address in the game's memory (as the game sees it, e.g. inside its .xbe image), the
 * bytes that must be there, "->", and the bytes written in their place (the same count). A patch is written only
 * when every one of its lines finds its bytes (or the new bytes, written before); when a line finds other bytes
 * (another version of the game, a part of the game that is not loaded yet) nothing of that patch is written and
 * it is tried again later. Checked every half second, so a part of the game loaded later, or the game started
 * again, gets the patch too.
 *
 * Game Vault's link chooses which patches are on: patches=<id>,<id> (only these), patches=none (none), no
 * patches= (each patch's own default).
 */

#include "qemu/osdep.h"
#include "qemu/xemu-patches.h"
#include "xemu-xbe.h"
#include "hw/hw.h"
#include "system/dma.h"
#include "system/hw_accel.h"
#include "cpu.h"
#include "exec/target_page.h"

#ifdef XEMU_UWP
#include "qemu/xemu-uwp.h"
#define patch_log(...) uwp_log(__VA_ARGS__)
#else
#define patch_log(...) fprintf(stderr, __VA_ARGS__), fputc('\n', stderr)
#endif

#define MAX_LINE_BYTES 64

static const struct {
    uint32_t title;
    const char *text;
} builtin_patches[] = {
#include "xemu-patches-builtin.inc"
    { 0, "" } /* keeps the list valid while it has no games */
};

typedef struct PatchLine {
    uint32_t addr;
    int len;
    uint8_t expect[MAX_LINE_BYTES];
    uint8_t value[MAX_LINE_BYTES];
} PatchLine;

typedef struct Patch {
    char *id;
    char *name;
    bool default_on;
    bool on;
    GArray *lines; /* PatchLine */
    int state;     /* 0 not yet, 1 written, 2 told that bytes differ */
    int writes;
    int wait;      /* checks to skip while a write is on its way */
} Patch;

static GPtrArray *patches;       /* Patch *, of the running game */
static uint32_t patch_title;     /* the game the list is for */
static uint32_t patch_timedate;  /* its .xbe build (a new .xbe of the same game reloads the list) */
static bool patch_loaded;
static char *link_choice;        /* patches= from the link; NULL: each patch's default */
static char *own_dir, *shipped_dir; /* both end with a separator */

void xemu_patches_set_dirs(const char *own, const char *shipped)
{
    g_free(own_dir);
    g_free(shipped_dir);
    own_dir = g_strdup(own ? own : "");
    shipped_dir = g_strdup(shipped ? shipped : "");
}

static void patch_free(gpointer p)
{
    Patch *pt = p;
    g_free(pt->id);
    g_free(pt->name);
    g_array_free(pt->lines, true);
    g_free(pt);
}

void xemu_patches_set_link_choice(const char *choice)
{
    g_free(link_choice);
    link_choice = choice ? g_strdup(choice) : NULL;
    patch_loaded = false; /* choose again */
}

static bool link_turns_on(const Patch *pt)
{
    if (!link_choice) {
        return pt->default_on;
    }
    if (!strcmp(link_choice, "none")) {
        return false;
    }
    if (!strcmp(link_choice, "all")) {
        return true;
    }
    gchar **ids = g_strsplit(link_choice, ",", -1);
    bool on = false;
    for (int i = 0; ids[i]; i++) {
        if (!g_ascii_strcasecmp(g_strstrip(ids[i]), pt->id)) {
            on = true;
        }
    }
    g_strfreev(ids);
    return on;
}

static int parse_bytes(const char *s, uint8_t *out)
{
    int n = 0;
    while (*s) {
        while (*s == ' ' || *s == '\t') {
            s++;
        }
        if (!*s) {
            break;
        }
        if (!g_ascii_isxdigit(s[0]) || !g_ascii_isxdigit(s[1]) || n >= MAX_LINE_BYTES) {
            return -1;
        }
        out[n++] = (g_ascii_xdigit_value(s[0]) << 4) | g_ascii_xdigit_value(s[1]);
        s += 2;
    }
    return n;
}

/* the text of one patch file; false (and the reason in the log) when it cannot be used */
static bool parse_file(const char *text, const char *path, uint32_t title, uint32_t version, GPtrArray *out)
{
    gchar **rows = g_strsplit(text, "\n", -1);
    Patch *cur = NULL;
    bool ok = true;
    for (int i = 0; ok && rows[i]; i++) {
        char *row = rows[i];
        char *hash = strchr(row, '#');
        if (hash) {
            *hash = '\0';
        }
        g_strstrip(row);
        if (!*row) {
            continue;
        }
        if (row[0] == '[') {
            char *end = strchr(row, ']');
            if (!end || end == row + 1) {
                patch_log("patches: %s line %d: a patch needs [id] name", path, i + 1);
                ok = false;
                break;
            }
            *end = '\0';
            cur = g_new0(Patch, 1);
            cur->id = g_strdup(g_strstrip(row + 1));
            cur->name = g_strdup(*g_strstrip(end + 1) ? g_strstrip(end + 1) : cur->id);
            cur->default_on = true;
            cur->lines = g_array_new(false, true, sizeof(PatchLine));
            g_ptr_array_add(out, cur);
            continue;
        }
        if (!g_ascii_strncasecmp(row, "title ", 6)) {
            if ((uint32_t)g_ascii_strtoull(row + 6, NULL, 16) != title) {
                patch_log("patches: %s is for title %s, the game is %08x", path, row + 6, title);
                ok = false;
            }
            continue;
        }
        if (!g_ascii_strncasecmp(row, "version ", 8)) {
            if ((uint32_t)g_ascii_strtoull(row + 8, NULL, 0) != version) {
                patch_log("patches: %s is for version %s, the game is 0x%08x: not used", path, row + 8, version);
                ok = false;
            }
            continue;
        }
        if (!g_ascii_strncasecmp(row, "default ", 8) && cur) {
            cur->default_on = !g_ascii_strcasecmp(g_strstrip(row + 8), "on");
            continue;
        }
        char *colon = strchr(row, ':');
        char *arrow = strstr(row, "->");
        if (cur && colon && arrow && colon < arrow) {
            PatchLine pl = { 0 };
            *colon = '\0';
            *arrow = '\0';
            char *end = NULL;
            pl.addr = (uint32_t)g_ascii_strtoull(row, &end, 16);
            int a = parse_bytes(colon + 1, pl.expect);
            int b = parse_bytes(arrow + 2, pl.value);
            if (!end || *end || a <= 0 || a != b) {
                patch_log("patches: %s line %d: needs 0xADDRESS: old bytes -> new bytes (same count)", path, i + 1);
                ok = false;
                break;
            }
            pl.len = a;
            g_array_append_val(cur->lines, pl);
            continue;
        }
        patch_log("patches: %s line %d not understood: %s", path, i + 1, row);
        ok = false;
    }
    g_strfreev(rows);
    for (guint i = 0; ok && i < out->len; i++) {
        Patch *pt = g_ptr_array_index(out, i);
        if (pt->lines->len == 0) {
            patch_log("patches: %s: [%s] has no lines", path, pt->id);
            ok = false;
        }
    }
    return ok;
}

static void load_for(uint32_t title, uint32_t version)
{
    g_clear_pointer(&patches, g_ptr_array_unref);
    patches = g_ptr_array_new_with_free_func(patch_free);

    char name[16];
    snprintf(name, sizeof(name), "%08X.txt", title);
    char *own = g_strdup_printf("%spatches" G_DIR_SEPARATOR_S "%s", own_dir ? own_dir : "", name);
    char *shipped = g_strdup_printf("%spatches" G_DIR_SEPARATOR_S "%s", shipped_dir ? shipped_dir : "", name);
    const char *path = g_file_test(own, G_FILE_TEST_EXISTS) ? own : shipped;
    gchar *text = NULL;
    if (!g_file_get_contents(path, &text, NULL, NULL)) {
        /* built in (gv-patches folder): these arrive with xemu's own updates */
        for (size_t i = 0; i < G_N_ELEMENTS(builtin_patches); i++) {
            if (builtin_patches[i].title == title) {
                text = g_strdup(builtin_patches[i].text);
                path = "built in";
            }
        }
    }
    if (text) {
        GPtrArray *list = g_ptr_array_new_with_free_func(patch_free);
        if (parse_file(text, path, title, version, list)) {
            g_ptr_array_unref(patches);
            patches = list;
            for (guint i = 0; i < patches->len; i++) {
                Patch *pt = g_ptr_array_index(patches, i);
                pt->on = link_turns_on(pt);
                patch_log("patches: %08x [%s] %s: %s (%u lines, from %s)", title, pt->id, pt->name,
                          pt->on ? "on" : "off", pt->lines->len, path == own ? "own folder" : path == shipped ? "xemu folder" : "built in");
            }
        } else {
            g_ptr_array_unref(list);
        }
        g_free(text);
    }
    g_free(own);
    g_free(shipped);
}

static bool virt_to_phys(uint32_t va, hwaddr *pa)
{
    CPUState *cs = qemu_get_cpu(0);
    if (!cs) {
        return false;
    }
    MemTxAttrs attrs;
    hwaddr gpa = cpu_get_phys_page_attrs_debug(cs, va & TARGET_PAGE_MASK, &attrs);
    if (gpa == -1) {
        return false;
    }
    *pa = gpa + (va & ~TARGET_PAGE_MASK);
    return true;
}

/* reads or writes guest memory at a game address, page by page; false where a page is not mapped */
static bool guest_rw(uint32_t va, uint8_t *buf, int len, bool write)
{
    while (len > 0) {
        hwaddr pa;
        if (!virt_to_phys(va, &pa)) {
            return false;
        }
        int n = MIN(len, (int)(TARGET_PAGE_SIZE - (pa & ~TARGET_PAGE_MASK)));
        /* a write through the address space also drops the CPU's translated copy of that code */
        if ((write ? dma_memory_write(&address_space_memory, pa, buf, n, MEMTXATTRS_UNSPECIFIED)
                   : dma_memory_read(&address_space_memory, pa, buf, n, MEMTXATTRS_UNSPECIFIED)) != MEMTX_OK) {
            return false;
        }
        va += n;
        buf += n;
        len -= n;
    }
    return true;
}

static void write_lines(CPUState *cs, run_on_cpu_data data)
{
    GArray *lines = data.host_ptr;
    for (guint i = 0; i < lines->len; i++) {
        PatchLine *pl = &g_array_index(lines, PatchLine, i);
        uint8_t back[MAX_LINE_BYTES];
        if (!guest_rw(pl->addr, pl->value, pl->len, true) || !guest_rw(pl->addr, back, pl->len, false) ||
            memcmp(back, pl->value, pl->len)) {
            patch_log("patches: 0x%08x could not be written", pl->addr);
        }
    }
    g_array_free(lines, true);
}

static void try_patch(Patch *pt)
{
    if (pt->wait > 0) {
        pt->wait--;
        return;
    }
    bool any_old = false;
    for (guint i = 0; i < pt->lines->len; i++) {
        PatchLine *pl = &g_array_index(pt->lines, PatchLine, i);
        uint8_t now[MAX_LINE_BYTES];
        if (!guest_rw(pl->addr, now, pl->len, false)) {
            return; /* not loaded yet */
        }
        if (!memcmp(now, pl->expect, pl->len)) {
            any_old = true;
        } else if (memcmp(now, pl->value, pl->len)) {
            if (pt->state != 2) {
                pt->state = 2;
                char got[3 * 8 + 1] = "";
                for (int k = 0; k < MIN(pl->len, 8); k++) {
                    snprintf(got + 3 * k, 4, "%02X ", now[k]);
                }
                patch_log("patches: [%s] waits: 0x%08x holds %s(another version, or not loaded yet)", pt->id,
                          pl->addr, got);
            }
            return;
        }
    }
    if (!any_old) {
        return; /* all written already */
    }
    /* written while the CPU is stopped between instructions: it may be running the very code */
    CPUState *cs = qemu_get_cpu(0);
    GArray *copy = g_array_copy(pt->lines);
    async_safe_run_on_cpu(cs, write_lines, RUN_ON_CPU_HOST_PTR(copy));
    pt->state = 1;
    pt->wait = 2;
    pt->writes++;
    patch_log("patches: [%s] %s written (%u lines%s)", pt->id, pt->name, pt->lines->len,
              pt->writes > 1 ? ", again: the game loaded that part again" : "");
}

void xemu_patches_tick(void)
{
    struct xbe *xbe = xemu_get_xbe_info();
    if (!xbe || !xbe->cert) {
        patch_title = 0;
        return;
    }
    uint32_t title = ldl_le_p(&xbe->cert->m_titleid);
    uint32_t timedate = ldl_le_p(&xbe->header->m_timedate);
    if (!patch_loaded || title != patch_title || timedate != patch_timedate) {
        patch_title = title;
        patch_timedate = timedate;
        patch_loaded = true;
        load_for(title, ldl_le_p(&xbe->cert->m_version));
    }
    for (guint i = 0; patches && i < patches->len; i++) {
        Patch *pt = g_ptr_array_index(patches, i);
        if (pt->on) {
            try_patch(pt);
        }
    }
}
