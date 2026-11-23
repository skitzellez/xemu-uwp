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
 * Patches made as IPS files for the game's default.xbe (the widescreen / 60 fps collections) use file offsets:
 *
 *   [widescreen] Widescreen 16:9
 *   version 0x00000001        (inside a patch: only this patch needs that version; others in the file still work)
 *   region 1                  (inside a patch: only a game whose certificate has one of these region bits, 1 = USA)
 *   file 0x0001A2B4 -> D9 05 00 10 2C 00
 *
 * A file line is placed with the running .xbe's own section table (file offset -> game address). It has no old
 * bytes to check, so its patch waits one second after the .xbe appears (the loader has copied the sections by then),
 * and it is written again when the game loads that part again (at most 20 times).
 *
 * Patches made as find-and-replace pairs (OgXHD's widescreen database) search the game's .xbe for their bytes:
 *
 *   [widescreen] Widescreen 16:9
 *   find 74 0A C7 44 24 10 -> EB 0A C7 44 24 10
 *
 * Each loaded section of the running .xbe is searched once, every match becomes a line with its old bytes checked
 * (old -> new, as above), so a release the pair was not made for gets it only where its bytes are. Other .xbe files
 * of the game (a title update on the hard disk: Halo 2 1.5) are searched too.
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

#define MAX_LINE_BYTES 512 /* OgXHD's widescreen database's longest pattern is 337 bytes */
#define MAX_FIND_LINES 4096

static const struct {
    uint32_t title;
    const char *text;
} builtin_patches[] = {
#include "xemu-patches-builtin.inc"
    { 0, "" } /* keeps the list valid while it has no games */
};

typedef struct PatchLine {
    uint32_t addr;
    uint32_t file_off; /* file lines: offset in the .xbe file; addr is found when the game's .xbe is known */
    bool from_file;
    bool found;        /* made from a find line: holding other bytes skips this line only, not the patch */
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
    bool has_file;     /* has file lines (no old bytes) */
    GArray *finds;     /* PatchLine: find lines, expect = the bytes searched for, value = their replacement */
    uint8_t scanned[256 / 8]; /* find lines: sections of the .xbe already searched */
    bool find_told;
    bool has_version;
    uint32_t version;  /* has_version: only this game version */
    uint32_t region;   /* nonzero: only a game with one of these certificate region bits */
    bool usable;       /* false: wrong version / region, or a file line outside the .xbe */
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
    g_array_free(pt->finds, true);
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
            cur->usable = true;
            cur->lines = g_array_new(false, true, sizeof(PatchLine));
            cur->finds = g_array_new(false, true, sizeof(PatchLine));
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
        if (!g_ascii_strncasecmp(row, "version ", 8) && cur) {
            cur->has_version = true;
            cur->version = (uint32_t)g_ascii_strtoull(row + 8, NULL, 0);
            continue;
        }
        if (!g_ascii_strncasecmp(row, "region ", 7) && cur) {
            cur->region = (uint32_t)g_ascii_strtoull(row + 7, NULL, 0);
            continue;
        }
        if (!g_ascii_strncasecmp(row, "file ", 5) && cur) {
            PatchLine pl = { 0 };
            char *arrow = strstr(row, "->");
            char *end = NULL;
            if (arrow) {
                *arrow = '\0';
                pl.file_off = (uint32_t)g_ascii_strtoull(row + 5, &end, 16);
                pl.len = parse_bytes(arrow + 2, pl.value);
            }
            if (!arrow || !end || *g_strstrip(end) || pl.len <= 0) {
                patch_log("patches: %s line %d: needs file 0xOFFSET -> new bytes", path, i + 1);
                ok = false;
                break;
            }
            pl.from_file = true;
            cur->has_file = true;
            g_array_append_val(cur->lines, pl);
            continue;
        }
        if (!g_ascii_strncasecmp(row, "find ", 5) && cur) {
            PatchLine pl = { 0 };
            char *arrow = strstr(row, "->");
            int a = -1, b = -1;
            if (arrow) {
                *arrow = '\0';
                a = parse_bytes(row + 5, pl.expect);
                b = parse_bytes(arrow + 2, pl.value);
            }
            if (a <= 0 || a != b) {
                patch_log("patches: %s line %d: needs find old bytes -> new bytes (same count, at most %d)", path,
                          i + 1, MAX_LINE_BYTES);
                ok = false;
                break;
            }
            pl.len = a;
            g_array_append_val(cur->finds, pl);
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
        if (pt->lines->len == 0 && pt->finds->len == 0) {
            patch_log("patches: %s: [%s] has no lines", path, pt->id);
            ok = false;
        }
    }
    return ok;
}

/* file offset in the running .xbe -> game address, with its own section table; false when not in a loaded part */
static bool file_to_addr(struct xbe *xbe, uint32_t off, int len, uint32_t *addr)
{
    uint32_t base = ldl_le_p(&xbe->header->m_base);
    uint32_t nsec = ldl_le_p(&xbe->header->m_sections);
    uint32_t sec_va = ldl_le_p(&xbe->header->m_section_headers_addr);
    if ((uint64_t)off + len <= xbe->headers_len) {
        *addr = base + off;
        return true;
    }
    for (uint32_t i = 0; i < nsec && i < 256; i++) {
        uint64_t at = (uint64_t)sec_va - base + i * 0x38u;
        if (sec_va < base || at + 0x38 > xbe->headers_len) {
            return false;
        }
        const uint8_t *sh = xbe->headers + at;
        uint32_t va = ldl_le_p(sh + 4), vsize = ldl_le_p(sh + 8), raw = ldl_le_p(sh + 12), rsize = ldl_le_p(sh + 16);
        if (off >= raw && (uint64_t)off + len <= (uint64_t)raw + MIN(rsize, vsize)) {
            *addr = va + (off - raw);
            return true;
        }
    }
    return false;
}

/* the first .xbe seen of each game is the disc's default.xbe: file lines are made for that one, not for other
 * .xbe files the game starts later (they have the same title ID) */
static uint32_t first_title, first_timedate;

static void check_usable(Patch *pt, struct xbe *xbe, uint32_t title, uint32_t version)
{
    uint32_t region = ldl_le_p(&xbe->cert->m_game_region);
    if (pt->has_file && ldl_le_p(&xbe->header->m_timedate) != first_timedate) {
        pt->usable = false;
        patch_log("patches: %08x [%s] is made for default.xbe, this is another .xbe of the game: not used", title,
                  pt->id);
        return;
    }
    if (pt->has_version && pt->version != version) {
        pt->usable = false;
        patch_log("patches: %08x [%s] is for version 0x%08x, the game is 0x%08x: not used", title, pt->id,
                  pt->version, version);
        return;
    }
    if (pt->region && !(pt->region & region)) {
        pt->usable = false;
        patch_log("patches: %08x [%s] is for region %x, the game is %x: not used", title, pt->id, pt->region, region);
        return;
    }
    for (guint i = 0; i < pt->lines->len; i++) {
        PatchLine *pl = &g_array_index(pt->lines, PatchLine, i);
        if (pl->from_file && !file_to_addr(xbe, pl->file_off, pl->len, &pl->addr)) {
            pt->usable = false;
            patch_log("patches: %08x [%s] file offset 0x%x is outside this .xbe's code and data: not used", title,
                      pt->id, pl->file_off);
            return;
        }
    }
    if (pt->has_file || pt->finds->len) {
        pt->wait = 2; /* one second: the loader copies the sections after the headers */
    }
}

static void load_for(uint32_t title, uint32_t version, struct xbe *xbe)
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
                check_usable(pt, xbe, title, version);
                pt->on = pt->on && pt->usable;
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

/* find lines: each section of the .xbe not searched yet is read from the game's memory and searched; every match
 * becomes a checked line. A section not in memory yet (loaded later by the game), or still all zeros, waits. */
static void scan_sections(Patch *pt, struct xbe *xbe)
{
    uint32_t base = ldl_le_p(&xbe->header->m_base);
    uint32_t nsec = ldl_le_p(&xbe->header->m_sections);
    uint32_t sec_va = ldl_le_p(&xbe->header->m_section_headers_addr);
    bool all = true;
    for (uint32_t i = 0; i < nsec && i < 256; i++) {
        if (pt->scanned[i / 8] & (1u << (i % 8))) {
            continue;
        }
        uint64_t at = (uint64_t)sec_va - base + i * 0x38u;
        if (sec_va < base || at + 0x38 > xbe->headers_len) {
            break;
        }
        const uint8_t *sh = xbe->headers + at;
        uint32_t va = ldl_le_p(sh + 4), len = MIN(ldl_le_p(sh + 8), ldl_le_p(sh + 16));
        if (len == 0 || len > 64 * 1024 * 1024) {
            pt->scanned[i / 8] |= 1u << (i % 8);
            continue;
        }
        uint8_t *buf = g_malloc(len);
        bool zero = true;
        if (guest_rw(va, buf, len, false)) {
            for (uint32_t k = 0; k < len && zero; k++) {
                zero = !buf[k];
            }
        }
        if (zero) { /* not readable, or not loaded yet */
            all = false;
            g_free(buf);
            continue;
        }
        pt->scanned[i / 8] |= 1u << (i % 8);
        int hits = 0;
        for (guint p = 0; p < pt->finds->len; p++) {
            const PatchLine *f = &g_array_index(pt->finds, PatchLine, p);
            for (uint32_t o = 0; o + f->len <= len && pt->lines->len < MAX_FIND_LINES; o++) {
                if (buf[o] != f->expect[0] || memcmp(buf + o, f->expect, f->len)) {
                    continue;
                }
                PatchLine pl = *f;
                pl.addr = va + o;
                pl.found = true;
                g_array_append_val(pt->lines, pl);
                hits++;
                o += f->len - 1;
            }
        }
        g_free(buf);
        if (hits) {
            patch_log("patches: [%s] section %u (0x%08x): %d matches of its %u patterns", pt->id, i, va, hits,
                      pt->finds->len);
        }
    }
    if (all && !pt->find_told) {
        pt->find_told = true;
        patch_log("patches: [%s] all sections searched: %u matches%s", pt->id, pt->lines->len,
                  pt->lines->len ? "" : " (made for another release of the game): not used");
    }
}

static void try_patch(Patch *pt, struct xbe *xbe)
{
    if (pt->wait > 0) {
        pt->wait--;
        return;
    }
    if (pt->finds->len) {
        scan_sections(pt, xbe);
        if (pt->lines->len == 0) {
            return;
        }
    }
    bool any_old = false;
    GArray *copy = g_array_new(false, true, sizeof(PatchLine));
    for (guint i = 0; i < pt->lines->len; i++) {
        PatchLine *pl = &g_array_index(pt->lines, PatchLine, i);
        uint8_t now[MAX_LINE_BYTES];
        if (!guest_rw(pl->addr, now, pl->len, false)) {
            if (pl->found) {
                continue; /* that part of the game is not in memory now */
            }
            g_array_free(copy, true);
            return; /* not loaded yet */
        }
        if (pl->found) {
            if (!memcmp(now, pl->expect, pl->len)) {
                any_old = true;
                g_array_append_val(copy, *pl);
            }
            continue; /* the new bytes, or other bytes there now (the game changed that place): left alone */
        }
        g_array_append_val(copy, *pl);
        if (pl->from_file) {
            if (memcmp(now, pl->value, pl->len)) {
                any_old = true; /* no old bytes to check: the version, region and place were checked */
            }
        } else if (!memcmp(now, pl->expect, pl->len)) {
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
            g_array_free(copy, true);
            return;
        }
    }
    if (!any_old) {
        g_array_free(copy, true);
        return; /* all written already */
    }
    if ((pt->has_file || pt->finds->len) && pt->writes >= 20) {
        if (pt->state != 3) {
            pt->state = 3;
            patch_log("patches: [%s] the game keeps changing those bytes back: left alone", pt->id);
        }
        g_array_free(copy, true);
        return;
    }
    /* written while the CPU is stopped between instructions: it may be running the very code */
    CPUState *cs = qemu_get_cpu(0);
    async_safe_run_on_cpu(cs, write_lines, RUN_ON_CPU_HOST_PTR(copy));
    pt->state = 1;
    pt->wait = 2;
    pt->writes++;
    patch_log("patches: [%s] %s written (%u lines%s)", pt->id, pt->name, copy->len,
              pt->writes > 1 ? ", again: the game loaded that part again" : "");
}

/*
 * Testing (finding what to patch): patches\watch.txt in the own folder lists game addresses, one per line,
 * "0xADDRESS length [label]"; each one is logged when its bytes change. Read once per game.
 */
typedef struct Watch {
    uint32_t addr;
    int len;
    char label[32];
    uint8_t last[64];
    bool seen;
} Watch;
static GArray *watches;

static void watch_load(void)
{
    g_clear_pointer(&watches, g_array_unref);
    char *path = g_strdup_printf("%spatches" G_DIR_SEPARATOR_S "watch.txt", own_dir ? own_dir : "");
    gchar *text = NULL;
    if (g_file_get_contents(path, &text, NULL, NULL)) {
        watches = g_array_new(false, true, sizeof(Watch));
        gchar **rows = g_strsplit(text, "\n", -1);
        for (int i = 0; rows[i]; i++) {
            Watch w = { 0 };
            char label[32] = "";
            if (sscanf(rows[i], "%x %d %31s", &w.addr, &w.len, label) >= 2 && w.len > 0 && w.len <= 64) {
                g_strlcpy(w.label, label, sizeof(w.label));
                g_array_append_val(watches, w);
            }
        }
        g_strfreev(rows);
        g_free(text);
        patch_log("patches: watching %u places (watch.txt)", watches->len);
    }
    g_free(path);
}

static void watch_tick(void)
{
    for (guint i = 0; watches && i < watches->len; i++) {
        Watch *w = &g_array_index(watches, Watch, i);
        uint8_t now[64];
        if (!guest_rw(w->addr, now, w->len, false)) {
            continue;
        }
        if (!w->seen || memcmp(now, w->last, w->len)) {
            char hex[64 * 3 + 1] = "";
            for (int k = 0; k < w->len; k++) {
                snprintf(hex + 3 * k, 4, "%02X ", now[k]);
            }
            patch_log("watch: %s 0x%08x = %s", w->label, w->addr, hex);
            memcpy(w->last, now, w->len);
            w->seen = true;
        }
    }
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
        if (title != first_title) {
            first_title = title;
            first_timedate = timedate;
        }
        patch_title = title;
        patch_timedate = timedate;
        patch_loaded = true;
        load_for(title, ldl_le_p(&xbe->cert->m_version), xbe);
        watch_load();
    }
    watch_tick();
    for (guint i = 0; patches && i < patches->len; i++) {
        Patch *pt = g_ptr_array_index(patches, i);
        if (pt->on) {
            try_patch(pt, xbe);
        }
    }
}
