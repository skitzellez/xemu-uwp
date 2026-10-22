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

#ifdef XEMU_UWP

void uwp_log(const char *fmt, ...);
bool xemu_uwp_test_file_exists(const char *name); /* ui/xemu.c */

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
    qemu_thread_create(&thread, "uwp_eip_sampler", sampler_thread, NULL, QEMU_THREAD_DETACHED);
    uwp_log("eip sampler: on (eip-sampler.txt)");
}

#endif
