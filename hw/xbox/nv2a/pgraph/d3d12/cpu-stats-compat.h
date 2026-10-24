/*
 * Copyright (c) 2026 Skitzelles - part of xemu for Xbox Dev Mode (github.com/skitzellez/xemu-uwp). GPL-2.0-or-later.
 *
 * New graphics renderer: stand-ins for CPU-side counters and switches the
 * renderer's tune file and log lines read in the build it came from
 * (danprice142's Xbox port of xemu, github.com/danprice142/xemu-uwp). Our CPU core has none of them: counters read 0 and the
 * switches do nothing, so the renderer code stays as it was written.
 */

#ifndef HW_NV2A_PGRAPH_D3D12_CPU_STATS_COMPAT_H
#define HW_NV2A_PGRAPH_D3D12_CPU_STATS_COMPAT_H

typedef struct XboxFastmemStats {
    uint64_t faults;
    uint64_t maps;
    uint64_t upgrades;
    uint64_t dirty;
    uint64_t patches;
    uint64_t unmaps;
    uint64_t protects;
    uint64_t flush_all;
    uint64_t sites;
    uint64_t patch_invalid;
    uint64_t patch_mmio;
    uint64_t patch_watch;
    uint64_t patch_not_ram;
    uint64_t patch_code;
    uint64_t patch_read_only;
    uint64_t patch_guard;
    uint64_t patch_map_failed;
} XboxFastmemStats;

static XboxFastmemStats xbox_fastmem_stats G_GNUC_UNUSED;
static bool xbox_fastmem_active G_GNUC_UNUSED;
static bool xbox_io_rewind G_GNUC_UNUSED;
static bool xbox_diag_log G_GNUC_UNUSED;

#define xbox_diag_printf(...)                                                 \
    do {                                                                      \
        if (xbox_diag_log) {                                                  \
            fprintf(stderr, __VA_ARGS__);                                     \
        }                                                                     \
    } while (0)

static int64_t qemu_vcpu_idle_us G_GNUC_UNUSED;
static int64_t qemu_vcpu_exec_us G_GNUC_UNUSED;
static int64_t qemu_vcpu_bql_us G_GNUC_UNUSED;
static int64_t qemu_vcpu_tlb_fills G_GNUC_UNUSED;
static int64_t qemu_mem_access_cb_flushes G_GNUC_UNUSED;
static int64_t qemu_tb_invalidates G_GNUC_UNUSED;
static int64_t qemu_tb_jmp_flushes G_GNUC_UNUSED;
static int64_t qemu_tb_jc_misses G_GNUC_UNUSED;
static int64_t qemu_tb_jc_miss_kind[3] G_GNUC_UNUSED;
static uint32_t qemu_tb_jc_flag_diff G_GNUC_UNUSED;
static int64_t qemu_tb_lookup_ptrs G_GNUC_UNUSED;
static int64_t qemu_tb_gens G_GNUC_UNUSED;
static int64_t qemu_tb_loop_kind[4] G_GNUC_UNUSED;
static int64_t qemu_tb_restores G_GNUC_UNUSED;
static int64_t qemu_tb_io_recompiles G_GNUC_UNUSED;
static int64_t tlb_page_flushes G_GNUC_UNUSED;
static int64_t tlb_large_page_flushes G_GNUC_UNUSED;
static int64_t tlb_reset_dirty_calls G_GNUC_UNUSED;
static int64_t tlb_reset_dirty_walks G_GNUC_UNUSED;

static inline void xbox_prof_start(const char *id, int seconds) {}
static inline void xbox_fastmem_set_codegen(bool on) {}
static inline void xbox_tb_set_inline_lookup(bool on) {}
static inline void xbox_set_sse_native(bool on) {}
static inline void tlb_set_dyn_min_bits(void *cpu, int bits) {}

#endif
