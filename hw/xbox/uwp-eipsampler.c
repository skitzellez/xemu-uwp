/*
 * Copyright (c) 2026 Skitzelles - part of xemu for Xbox Dev Mode (github.com/skitzellez/xemu-uwp). GPL-2.0-or-later.
 *
 * Where the emulated CPU spends its time (testing only). With eip-sampler.txt next to the log
 * (LocalState/xemu/xemu, put there through the Device Portal) a thread asks the emulated CPU about
 * 1000 times a second, at a point where its registers are up to date, which address it is about to run.
 * Every 2 seconds the log gets the hottest addresses and their share, and the first time an address is
 * among them, the 96 bytes of code there (to disassemble on a PC). Busy-wait loops show up as a few
 * addresses holding most of the time while the game draws little.
 */
#include "qemu/osdep.h"
#include "qemu/thread.h"
#include "qemu/atomic.h"
#include "qemu/timer.h"
#include "hw/core/cpu.h"
#include "system/runstate.h"
#include "cpu.h"
#include "tcg/tcg.h"

#ifdef XEMU_UWP
#include <windows.h>

void uwp_log(const char *fmt, ...);
bool xemu_uwp_test_file_exists(const char *name); /* ui/xemu.c */

/* Whether a test file in LocalState/xemu/xemu holds a word. */
static bool xemu_uwp_test_file_contains(const char *name, const char *word)
{
    extern const char *xemu_settings_get_base_path(void);
    char *path = g_strdup_printf("%s%s", xemu_settings_get_base_path(), name);
    gchar *text = NULL;
    bool found = g_file_get_contents(path, &text, NULL, NULL) && strstr(text, word) != NULL;
    g_free(text);
    g_free(path);
    return found;
}

#define REPORT_SAMPLES 2000
#define TOP 12

static GHashTable *counts;      /* linear address -> samples, vCPU thread only */
static GHashTable *dumped;      /* addresses whose code is already in the log */
static GHashTable *stacks;      /* linear address -> the top of the stack at its last sample (8 dwords) */
static unsigned total, halted;
static int pending;

typedef struct { guint pc, count; } Place;

static int by_count(const void *a, const void *b)
{
    const Place *pa = a, *pb = b;
    return pa->count < pb->count ? 1 : pa->count > pb->count ? -1 : 0;
}

static void report(CPUState *cs)
{
    guint n_places = g_hash_table_size(counts);
    Place *places = g_new(Place, n_places ? n_places : 1);
    GHashTableIter it;
    gpointer key, value;
    guint n = 0;
    g_hash_table_iter_init(&it, counts);
    while (g_hash_table_iter_next(&it, &key, &value)) {
        places[n].pc = GPOINTER_TO_UINT(key);
        places[n++].count = GPOINTER_TO_UINT(value);
    }
    qsort(places, n, sizeof(Place), by_count);
    GString *line = g_string_new(NULL);
    g_string_printf(line, "eip: %u samples, halted %u%%, %u places |", total, halted * 100 / total, n);
    for (guint i = 0; i < n && i < TOP; i++) {
        g_string_append_printf(line, " %08x %u%%", places[i].pc, places[i].count * 100 / total);
    }
    uwp_log("%s", line->str);
    {
        extern unsigned xemu_uwp_vblank_count, xemu_uwp_ctxsw_count, xemu_uwp_error_count;
        extern uint64_t xemu_uwp_ctxsw_wait_ns, xemu_uwp_error_wait_ns, xemu_uwp_error_bh_ns;
        static int64_t last;
        int64_t now = get_clock_realtime();
        double secs = last ? (now - last) / 1e9 : 0;
        unsigned v = qatomic_xchg(&xemu_uwp_vblank_count, 0), c = qatomic_xchg(&xemu_uwp_ctxsw_count, 0),
                 e = qatomic_xchg(&xemu_uwp_error_count, 0);
        uint64_t cw = qatomic_xchg(&xemu_uwp_ctxsw_wait_ns, 0), ew = qatomic_xchg(&xemu_uwp_error_wait_ns, 0);
        uint64_t eb = qatomic_xchg(&xemu_uwp_error_bh_ns, 0);
        last = now;
        uwp_log("gpu irq: %.1f s | vblank %u | context switch %u, GPU stopped %.1f ms (avg %.2f) | "
                "software method %u, GPU stopped %.1f ms (avg %.2f, of which raising %.2f)", secs, v, c,
                cw / 1e6, c ? cw / 1e6 / c : 0, e, ew / 1e6, e ? ew / 1e6 / e : 0, e ? eb / 1e6 / e : 0);
    }
    /* code of the hottest places, once each */
    for (guint i = 0; i < n && i < 4; i++) {
        gpointer k = GUINT_TO_POINTER(places[i].pc);
        if (g_hash_table_contains(dumped, k) || g_hash_table_size(dumped) > 200) {
            continue;
        }
        g_hash_table_add(dumped, k);
        uint8_t code[96];
        if (cpu_memory_rw_debug(cs, places[i].pc, code, sizeof(code), false) == 0) {
            GString *hex = g_string_new(NULL);
            for (size_t j = 0; j < sizeof(code); j++) {
                g_string_append_printf(hex, "%02x", code[j]);
            }
            uwp_log("eip code %08x: %s", places[i].pc, hex->str);
            uint32_t *st = g_hash_table_lookup(stacks, k);
            if (st) {
                uwp_log("eip stack %08x: %08x %08x %08x %08x %08x %08x %08x %08x", places[i].pc, st[0], st[1], st[2],
                        st[3], st[4], st[5], st[6], st[7]);
                uwp_log("eip regs %08x: eax %08x ecx %08x edx %08x ebx %08x esp %08x ebp %08x esi %08x edi %08x"
                        " | mxcsr %04x fpuc %04x",
                        places[i].pc, st[8], st[9], st[10], st[11], st[12], st[13], st[14], st[15],
                        (unsigned)cpu_env(cs)->mxcsr, (unsigned)cpu_env(cs)->fpuc);
                /* code before stack words that look like kernel return addresses: the call sites */
                for (int w = 0; w < 8; w++) {
                    uint8_t site[64];
                    /* kernel code, or game code (XBE sections load from 0x10000) */
                    bool code = (st[w] >= 0x80010000u && st[w] < 0x80080000u) || (st[w] >= 0x11000u && st[w] < 0x800000u);
                    if (!code ||
                        cpu_memory_rw_debug(cs, st[w] - 48, site, sizeof(site), false) != 0) {
                        continue;
                    }
                    GString *h = g_string_new(NULL);
                    for (size_t j = 0; j < sizeof(site); j++) {
                        g_string_append_printf(h, "%02x", site[j]);
                    }
                    uwp_log("eip site %08x (from %08x): %s", st[w] - 48, places[i].pc, h->str);
                    g_string_free(h, true);
                }
            }
            g_string_free(hex, true);
        }
    }
    g_string_free(line, true);
    g_free(places);
    g_hash_table_remove_all(counts);
    total = halted = 0;
}

static void sample(CPUState *cs, run_on_cpu_data data)
{
    CPUX86State *env = cpu_env(cs);
    if (cs->halted) {
        halted++;
    } else {
        guint pc = (guint)(env->segs[R_CS].base + env->eip);
        gpointer key = GUINT_TO_POINTER(pc);
        g_hash_table_replace(counts, key,
                             GUINT_TO_POINTER(GPOINTER_TO_UINT(g_hash_table_lookup(counts, key)) + 1));
        /* the stack names the callers of a hot loop (return addresses among these words) */
        uint32_t *st = g_hash_table_lookup(stacks, key);
        if (!st && g_hash_table_size(stacks) < 4096) {
            st = g_new0(uint32_t, 16);
            g_hash_table_insert(stacks, key, st);
        }
        if (st) {
            target_ulong sp = env->segs[R_SS].base + env->regs[R_ESP];
            if (cpu_memory_rw_debug(cs, sp, st, 8 * sizeof(uint32_t), false) != 0) {
                memset(st, 0, 8 * sizeof(uint32_t));
            }
            for (int r = 0; r < 8; r++) {
                st[8 + r] = (uint32_t)env->regs[r]; /* eax ecx edx ebx esp ebp esi edi */
            }
        }
    }
    if (++total >= REPORT_SAMPLES) {
        report(cs);
    }
    qatomic_set(&pending, 0);
}

/*
 * "host" in eip-sampler.txt: no waiting for the CPU. Asking it at its next safe point lands on the
 * kernel's 1 kHz clock interrupt far too often (007 Everything or Nothing: 55% "in" the clock ISR). This
 * pauses the vCPU thread itself for an instant, reads where it is, and splits the samples into translated
 * guest code (counted by guest address, as above) and xemu's own code (helpers, devices), counted by
 * offset in the exe ("eip host" lines; look the offsets up in the unstripped exe with addr2line).
 */
void *xemu_uwp_thread_handle(const char *name); /* ui/xemu.c */
static GHashTable *host_counts;  /* exe offset / 256 -> samples */
static unsigned host_total, host_xemu;
/* the GPU thread (pfifo), sampled the same way: exe offsets, other modules by name */
static GHashTable *gpu_counts, *module_counts, *gpu_module_counts;
static unsigned gpu_total;

/* "exe+<offset>" for xemu's own code, else the module's file name (a driver, Windows) */
static uint64_t exe_start, exe_end;
static GHashTable *callers, *gpu_callers;  /* "module<exe+off" -> samples */

/* The first return address into xemu's own code on a paused thread's stack. */
static uint64_t exe_caller(uint64_t rsp)
{
    const uint64_t *stack = (const uint64_t *)(uintptr_t)rsp;
    MEMORY_BASIC_INFORMATION mbi;
    int words = 96;

    /* stop at the end of the stack's committed memory: a paused thread near
     * the top of its stack had the walk read past it (sampler crashed) */
    if (!rsp || !VirtualQuery((LPCVOID)(uintptr_t)rsp, &mbi, sizeof(mbi)) ||
        mbi.State != MEM_COMMIT) {
        return 0;
    }
    words = MIN(words, (int)(((uintptr_t)mbi.BaseAddress + mbi.RegionSize -
                              (uintptr_t)rsp) / sizeof(uint64_t)));
    for (int i = 0; i < words; i++) {
        uint64_t v = stack[i];
        if (v >= exe_start && v < exe_end) {
            return v;
        }
    }
    return 0;
}

static void count_caller(GHashTable *table, const char *module, uint64_t caller)
{
    char key[96];
    snprintf(key, sizeof(key), "%s<exe+%llx", module, (unsigned long long)(caller ? caller - exe_start : 0));
    gpointer old = g_hash_table_lookup(table, key);
    g_hash_table_replace(table, g_strdup(key), GUINT_TO_POINTER(GPOINTER_TO_UINT(old) + 1));
}

/* caller: the first xemu return address on its stack, read while it was paused (or 0) */
static void count_host_pc(uint64_t rip, uint64_t caller, HMODULE exe, GHashTable *by_offset, GHashTable *by_module,
                          GHashTable *by_caller)
{
    HMODULE module = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)(uintptr_t)rip, &module);
    if (module == exe) {
        gpointer key = GUINT_TO_POINTER((guint)((rip - (uintptr_t)exe) / 256));
        g_hash_table_replace(by_offset, key,
                             GUINT_TO_POINTER(GPOINTER_TO_UINT(g_hash_table_lookup(by_offset, key)) + 1));
        return;
    }
    char name[64] = "unknown";
    if (module) {
        WCHAR wide[MAX_PATH];
        DWORD n = GetModuleFileNameW(module, wide, MAX_PATH);
        if (n) {
            WCHAR *base = wide + n;
            while (base > wide && base[-1] != L'\\' && base[-1] != L'/') {
                base--;
            }
            WideCharToMultiByte(CP_UTF8, 0, base, -1, name, sizeof(name), NULL, NULL);
        }
    }
    gpointer old = g_hash_table_lookup(by_module, name);
    g_hash_table_replace(by_module, g_strdup(name), GUINT_TO_POINTER(GPOINTER_TO_UINT(old) + 1));
    count_caller(by_caller, name, caller);
}

/* top entries of a table, as " <label> <pct>%" */
static void append_top(GString *line, GHashTable *table, unsigned total, bool modules)
{
    GHashTableIter it;
    gpointer key, value;
    guint n = 0, size = g_hash_table_size(table);
    struct { gpointer key; guint count; } *v = g_new(__typeof__(*v), size ? size : 1);
    g_hash_table_iter_init(&it, table);
    while (g_hash_table_iter_next(&it, &key, &value)) {
        v[n].key = key;
        v[n++].count = GPOINTER_TO_UINT(value);
    }
    for (guint a = 0; a < n; a++) {
        for (guint b = a + 1; b < n; b++) {
            if (v[b].count > v[a].count) {
                __typeof__(*v) t = v[a];
                v[a] = v[b];
                v[b] = t;
            }
        }
    }
    for (guint i = 0; i < n && i < (modules ? 10 : TOP); i++) {
        if (modules) {
            g_string_append_printf(line, " %s %u%%", (const char *)v[i].key, v[i].count * 100 / MAX(total, 1));
        } else {
            g_string_append_printf(line, " exe+%x %u%%", GPOINTER_TO_UINT(v[i].key) * 256,
                                   v[i].count * 100 / MAX(total, 1));
        }
    }
    g_free(v);
}

static void host_report(CPUState *cs)
{
    GHashTableIter it;
    gpointer key, value;
    guint n = 0, size = g_hash_table_size(host_counts);
    Place *places = g_new(Place, size ? size : 1);
    g_hash_table_iter_init(&it, host_counts);
    while (g_hash_table_iter_next(&it, &key, &value)) {
        places[n].pc = GPOINTER_TO_UINT(key);
        places[n++].count = GPOINTER_TO_UINT(value);
    }
    qsort(places, n, sizeof(Place), by_count);
    GString *line = g_string_new(NULL);
    g_string_printf(line, "eip host: %u samples, in translated guest code %u%%, in xemu %u%% |", host_total,
                    (host_total - host_xemu) * 100 / MAX(host_total, 1), host_xemu * 100 / MAX(host_total, 1));
    for (guint i = 0; i < n && i < TOP; i++) {
        g_string_append_printf(line, " exe+%x %u%%", places[i].pc * 256, places[i].count * 100 / host_total);
    }
    g_string_append(line, " | other modules:");
    append_top(line, module_counts, host_total, true);
    uwp_log("%s", line->str);
    g_string_printf(line, "eip cpu thread waits/copies from:");
    append_top(line, callers, host_total, true);
    uwp_log("%s", line->str);
    g_string_printf(line, "eip gpu thread: %u samples |", gpu_total);
    append_top(line, gpu_counts, gpu_total, false);
    g_string_append(line, " | other modules:");
    append_top(line, gpu_module_counts, gpu_total, true);
    uwp_log("%s", line->str);
    g_string_printf(line, "eip gpu thread waits/copies from:");
    append_top(line, gpu_callers, gpu_total, true);
    uwp_log("%s", line->str);
    g_hash_table_remove_all(callers);
    g_hash_table_remove_all(gpu_callers);
    g_string_free(line, true);
    g_free(places);
    g_hash_table_remove_all(host_counts);
    g_hash_table_remove_all(module_counts);
    g_hash_table_remove_all(gpu_counts);
    g_hash_table_remove_all(gpu_module_counts);
    host_total = host_xemu = gpu_total = 0;
    report(cs);
}

static void *host_sampler_thread(void *opaque)
{
    HMODULE exe = GetModuleHandleW(NULL);
    {
        const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)exe;
        const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)((const char *)exe + dos->e_lfanew);
        exe_start = (uint64_t)(uintptr_t)exe;
        exe_end = exe_start + nt->OptionalHeader.SizeOfImage;
    }
    for (;;) {
        g_usleep(1000);
        CPUState *cs = first_cpu;
        HANDLE h = xemu_uwp_thread_handle("vcpu");
        if (!cs || !h || !runstate_is_running()) {
            continue;
        }
        CONTEXT ctx = { .ContextFlags = CONTEXT_CONTROL };
        uint64_t rip = 0, caller = 0;
        if (SuspendThread(h) == (DWORD)-1) {
            continue;
        }
        if (GetThreadContext(h, &ctx)) {
            rip = ctx.Rip;
            /* only reads while it is paused: no allocation (it may hold the heap lock) */
            if (!(rip >= exe_start && rip < exe_end)) {
                caller = exe_caller(ctx.Rsp);
            }
        }
        CPUX86State *env = cpu_env(cs);
        bool is_halted = cs->halted;
        guint pc = (guint)(env->segs[R_CS].base + env->eip);
        uint32_t regs[8];
        for (int r = 0; r < 8; r++) {
            regs[r] = (uint32_t)env->regs[r];
        }
        target_ulong sp = env->segs[R_SS].base + env->regs[R_ESP];
        ResumeThread(h);
        if (!rip) {
            continue;
        }
        if (is_halted) {
            halted++;
        } else if (in_code_gen_buffer((void *)(uintptr_t)rip)) {
            gpointer key = GUINT_TO_POINTER(pc);
            g_hash_table_replace(counts, key,
                                 GUINT_TO_POINTER(GPOINTER_TO_UINT(g_hash_table_lookup(counts, key)) + 1));
            uint32_t *st = g_hash_table_lookup(stacks, key);
            if (!st && g_hash_table_size(stacks) < 4096) {
                st = g_new0(uint32_t, 16);
                g_hash_table_insert(stacks, key, st);
            }
            if (st) {
                if (cpu_memory_rw_debug(cs, sp, st, 8 * sizeof(uint32_t), false) != 0) {
                    memset(st, 0, 8 * sizeof(uint32_t));
                }
                memcpy(st + 8, regs, sizeof(regs));
            }
        } else {
            count_host_pc(rip, caller, exe, host_counts, module_counts, callers);
            host_xemu++;
        }
        /* the GPU thread at the same moment */
        HANDLE g = xemu_uwp_thread_handle("pfifo");
        if (g && SuspendThread(g) != (DWORD)-1) {
            CONTEXT gc = { .ContextFlags = CONTEXT_CONTROL };
            uint64_t grip = 0, gcaller = 0;
            if (GetThreadContext(g, &gc)) {
                grip = gc.Rip;
                if (!(grip >= exe_start && grip < exe_end)) {
                    gcaller = exe_caller(gc.Rsp);
                }
            }
            ResumeThread(g);
            if (grip) {
                count_host_pc(grip, gcaller, exe, gpu_counts, gpu_module_counts, gpu_callers);
                gpu_total++;
            }
        }
        total++;
        if (++host_total >= REPORT_SAMPLES) {
            host_report(cs);
        }
    }
    return NULL;
}

static void *sampler_thread(void *opaque)
{
    for (;;) {
        g_usleep(1000);
        CPUState *cs = first_cpu;
        if (!cs || !runstate_is_running() || qatomic_read(&pending)) {
            continue;
        }
        qatomic_set(&pending, 1);
        async_run_on_cpu(cs, sample, RUN_ON_CPU_NULL);
    }
    return NULL;
}

/*
 * For the start screen (ui/xui/main.cc, every player): has the game's own code run yet? The BIOS, its start-up
 * animation and the kernel all run at 0x80000000 and up; a game's code sits below. One look per call, only while
 * the start screen asks (once a screen frame), so it costs nothing once the game runs.
 */
static int game_code_pending, game_code_seen;

static void look_for_game_code(CPUState *cs, run_on_cpu_data data)
{
    CPUX86State *env = cpu_env(cs);
    uint32_t pc = (uint32_t)(env->segs[R_CS].base + env->eip);
    if (!cs->halted && pc >= 0x10000 && pc < 0x80000000u) {
        qatomic_set(&game_code_seen, 1);
    }
    qatomic_set(&game_code_pending, 0);
}

bool xemu_uwp_game_code_seen(void)
{
    if (qatomic_read(&game_code_seen)) {
        return true;
    }
    CPUState *cs = first_cpu;
    if (cs && runstate_is_running() && !qatomic_read(&game_code_pending)) {
        qatomic_set(&game_code_pending, 1);
        async_run_on_cpu(cs, look_for_game_code, RUN_ON_CPU_NULL);
    }
    return false;
}

void xemu_uwp_eip_sampler_start(void)
{
    static QemuThread thread;
    if (!xemu_uwp_test_file_exists("eip-sampler.txt")) {
        return;
    }
    counts = g_hash_table_new(NULL, NULL);
    dumped = g_hash_table_new(NULL, NULL);
    stacks = g_hash_table_new_full(NULL, NULL, NULL, g_free);
    host_counts = g_hash_table_new(NULL, NULL);
    gpu_counts = g_hash_table_new(NULL, NULL);
    module_counts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    gpu_module_counts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    callers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    gpu_callers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    if (xemu_uwp_test_file_contains("eip-sampler.txt", "host")) {
        qemu_thread_create(&thread, "uwp_eip_sampler", host_sampler_thread, NULL, QEMU_THREAD_DETACHED);
        uwp_log("eip sampler: on, pausing the vCPU thread (eip-sampler.txt says host)");
        return;
    }
    qemu_thread_create(&thread, "uwp_eip_sampler", sampler_thread, NULL, QEMU_THREAD_DETACHED);
    uwp_log("eip sampler: on (eip-sampler.txt)");
}

#endif
