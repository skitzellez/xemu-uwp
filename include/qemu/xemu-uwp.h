/*
 * Copyright (c) 2026 Skitzelles - part of xemu for Xbox Dev Mode (github.com/skitzellez/xemu-uwp). GPL-2.0-or-later.
 *
 * Helpers for the Xbox (UWP, Dev Mode) build of xemu.
 *
 * Xbox apps run in an app container: executable memory must come from the
 * *FromApp memory functions (with the codeGeneration capability). MinGW only
 * declares those for _WIN32_WINNT >= Windows 10 while QEMU targets Windows 8,
 * so they are looked up at run time from kernelbase.dll.
 *
 * Copyright (c) 2026 Skitzelles (https://github.com/skitzellez/xemu-uwp)
 * Licensed under the GNU GPL, version 2 or later, like the rest of xemu.
 */

#ifndef QEMU_XEMU_UWP_H
#define QEMU_XEMU_UWP_H

#ifdef XEMU_UWP

#include <windows.h>

typedef PVOID (WINAPI *xemu_uwp_VirtualAllocFromApp_fn)(PVOID, SIZE_T, ULONG,
                                                         ULONG);
typedef BOOL (WINAPI *xemu_uwp_VirtualProtectFromApp_fn)(PVOID, SIZE_T, ULONG,
                                                          PULONG);

static inline FARPROC xemu_uwp_kernelbase_proc(const char *name)
{
    HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
    return kb ? GetProcAddress(kb, name) : NULL;
}

/*
 * CreateFileW works in an app container only where the package itself has
 * been granted access (its own folders, or an NTFS volume with an ACL for
 * ALL APPLICATION PACKAGES). CreateFileFromAppW goes through the storage
 * broker when direct access is refused, which is what makes drives without
 * that ACL readable with the broadFileSystemAccess capability. The handle
 * is an ordinary file handle (ReadFile, overlapped I/O, SetFilePointerEx).
 */
typedef HANDLE (WINAPI *xemu_uwp_CreateFileFromAppW_fn)(
    LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);

/* ui/xemu.c: called once the drives exist; installs the dashboard from the game disc if needed */
void xemu_uwp_dashboard_from_disc(void);

/* Counters for the memory report (ui/xemu.c): GPU objects made by the renderer. */
enum {
    UWP_CNT_QUERIES_PENDING, /* pixel-count queries waiting for the game to read them (a level, not a rate) */
    UWP_CNT_QUERIES_FOLDED,  /* queries read back early because too many were waiting */
    UWP_CNT_TEX_NEW,
    UWP_CNT_TEX_DEL,
    UWP_CNT_SURF_NEW,
    UWP_CNT_SURF_DEL,
    UWP_CNT_STREAM_WRAP,     /* 64 MB vertex stream buffer replaced */
    UWP_CNT_VERTEX_REPACK,   /* unaligned vertex attributes copied aligned (gl/vertex.c) */
    UWP_CNT_GL_ERRORS,
    UWP_CNT_TB_LOOKUP,       /* translated-block lookups outside direct chaining (accel/tcg/cpu-exec.c) */
    UWP_CNT_TB_JC_MISS,      /* ... not in the jump cache */
    UWP_CNT_TB_HT_MISS,      /* ... not translated at all */
    UWP_CNT_JC_FLUSH,        /* whole jump cache emptied */
    UWP_CNT_JC_PAGE,         /* jump cache entries of one page dropped */
    UWP_CNT_TB_LOOP,         /* ... of which from the main loop (a block ended without jumping on) */
    UWP_CNT_MAX
};
extern unsigned int xemu_uwp_counters[UWP_CNT_MAX];

/* ui/xemu.c: the disc image Game Vault shared through a token, when the normal open is refused */
HANDLE xemu_uwp_shared_file(LPCWSTR name, DWORD access);
/* one log line about opening a file the normal way failed (ui/xemu.c) */
void xemu_uwp_open_note(LPCWSTR name, const char *what);

static inline HANDLE xemu_uwp_create_file_w(LPCWSTR name, DWORD access,
                                            DWORD share, DWORD creation,
                                            DWORD flags)
{
    /*
     * The disc Game Vault shared with us comes from that share first: asking the file broker for a drive this app
     * has no consent for can wait for a consent prompt an Xbox never shows, until the console ends xemu (games on
     * a 1 TB USB disk did not start at all after a reinstall, 2026-10-16).
     */
    if (!(access & GENERIC_WRITE)) {
        HANDLE shared = xemu_uwp_shared_file(name, access);
        if (shared != INVALID_HANDLE_VALUE) {
            return shared;
        }
    }
    xemu_uwp_CreateFileFromAppW_fn fn = (xemu_uwp_CreateFileFromAppW_fn)
        xemu_uwp_kernelbase_proc("CreateFileFromAppW");
    if (fn) {
        /* the broker takes only backslashes; xemu's paths often have '/' */
        wchar_t fixed[MAX_PATH * 4];
        if (wcslen(name) < ARRAYSIZE(fixed)) {
            wchar_t *o = fixed;
            for (LPCWSTR c = name; *c; c++) {
                *o++ = *c == L'/' ? L'\\' : *c;
            }
            *o = 0;
            name = fixed;
        }
        bool usb = name[0] && name[1] == L':' && name[0] != L'Q' && name[0] != L'q';
        if (usb) {
            xemu_uwp_open_note(name, access & GENERIC_WRITE ? "asking the broker (write)" : "asking the broker (read)");
        }
        HANDLE h = fn(name, access, share, NULL, creation, flags, NULL);
        if (usb) {
            xemu_uwp_open_note(name, h != INVALID_HANDLE_VALUE ? "broker: opened" : "broker: refused");
        }
        if (h != INVALID_HANDLE_VALUE) {
            return h;
        }
    }
    HANDLE h = CreateFileW(name, access, share, NULL, creation, flags, NULL);
    if (h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED) {
        xemu_uwp_open_note(name, "refused, trying Game Vault's share");
        /* the disc Game Vault shared with us (ui/xemu.c) */
        HANDLE shared = xemu_uwp_shared_file(name, access);
        if (shared != INVALID_HANDLE_VALUE) {
            return shared;
        }
        SetLastError(ERROR_ACCESS_DENIED);
    }
    return h;
}

static inline PVOID xemu_uwp_virtual_alloc(PVOID addr, SIZE_T size,
                                           ULONG type, ULONG protect)
{
    xemu_uwp_VirtualAllocFromApp_fn fn = (xemu_uwp_VirtualAllocFromApp_fn)
        xemu_uwp_kernelbase_proc("VirtualAllocFromApp");
    if (!fn) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return NULL;
    }
    return fn(addr, size, type, protect);
}

static inline BOOL xemu_uwp_virtual_protect(PVOID addr, SIZE_T size,
                                            ULONG protect, PULONG old)
{
    xemu_uwp_VirtualProtectFromApp_fn fn = (xemu_uwp_VirtualProtectFromApp_fn)
        xemu_uwp_kernelbase_proc("VirtualProtectFromApp");
    if (!fn) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return FALSE;
    }
    return fn(addr, size, protect, old);
}

/*
 * Lightweight profiler for the Xbox port: wall-clock timers around the places
 * that can block on the GPU, plus CPU time per registered thread. The UI loop
 * prints the totals in its heartbeat line (uwp_debug.log).
 */
#define UWP_PROF_TIMERS_XMAC     _X(PF_IDLE) _X(PF_SYNC) _X(PF_FLIP_FINISH) _X(PF_SURF_DOWN) _X(PF_SURF_UP)     _X(PF_TEX_UP) _X(PF_SHADER_GEN) _X(PF_DRAW) _X(PF_PUSHER) _X(PF_BLIT) _X(PF_CLEAR) _X(PF_BIND_TEX) _X(PF_S2T) _X(PF_SURF_UPD) _X(PF_SU_DIRTY) _X(PF_SU_EVICT) _X(PF_SU_NEW) _X(PF_SU_FBO) _X(PF_SU_UPLOAD) _X(PF_SU_OLD) _X(PF_WB_START) _X(PF_ZFILL) _X(PF_BIND_SHADER) _X(PF_FLUSH_DRAW) _X(PF_GLDRAW) _X(CPU_EXEC) _X(CPU_HALT) _X(CPU_BQL) _X(TB_GEN) _X(TB_FLUSH) _X(TB_INVAL) _X(DVD_READ) _X(SURF_CPU_ACCESS) _X(ML_POLL) _X(ML_SPIN) _X(ML_WORK) _X(ML_SELECT_ERR) _X(VC_FIFO_LOCK) _X(VC_GRAPH_LOCK) _X(PF_PENDING) _X(PF_REPORTS)     _X(UI_GETFB) _X(UI_HUD) _X(UI_FINISH) _X(UI_SWAP)

enum {
#define _X(x) UWP_PROF_##x,
    UWP_PROF_TIMERS_XMAC
#undef _X
    UWP_PROF__COUNT
};

extern int64_t uwp_prof_ticks[UWP_PROF__COUNT];
extern int uwp_prof_calls[UWP_PROF__COUNT];
void uwp_prof_register_thread(const char *name);

static inline int64_t uwp_prof_now(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

#define UWP_PROF_BEGIN(var) int64_t var = uwp_prof_now()
#define UWP_PROF_END(id, var) do {     uwp_prof_ticks[UWP_PROF_##id] += uwp_prof_now() - (var);     uwp_prof_calls[UWP_PROF_##id]++; } while (0)
#define UWP_PROF_THREAD(name) uwp_prof_register_thread(name)
/* Times the rest of the enclosing block, whichever way it is left. */
static inline void uwp_prof_scope_end(int64_t (*scope)[2])
{
    uwp_prof_ticks[(*scope)[0]] += uwp_prof_now() - (*scope)[1];
    uwp_prof_calls[(*scope)[0]]++;
}
#define UWP_PROF_SCOPE(id) \
    int64_t uwp_scope_##id[2] __attribute__((cleanup(uwp_prof_scope_end))) = \
        { UWP_PROF_##id, uwp_prof_now() }
#define UWP_PROF_COUNT(id) (uwp_prof_calls[UWP_PROF_##id]++)

/* Why a surface was read back from the GPU (set by the caller, pfifo thread). */
enum {
    UWP_DL_CPU_ACCESS, UWP_DL_TEX_OVERLAP, UWP_DL_EVICT_OVERLAP,
    UWP_DL_EVICT_OLD, UWP_DL_EVICT_INCOMPATIBLE, UWP_DL_BLIT, UWP_DL_ALL_DIRTY,
    UWP_DL_S2T_SLOW, UWP_DL__COUNT
};
extern int uwp_dl_site;
extern int uwp_dl_counts[UWP_DL__COUNT];
void uwp_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* the app's memory: in use and still allowed by the console (bytes); false when it cannot be read */
bool xemu_uwp_memory(uint64_t *used, uint64_t *left);
/* the render scale to use: the setting held to what the TV shows and the memory allows (logs when lowered) */
unsigned int xemu_uwp_cap_surface_scale(unsigned int want);
#define UWP_DL_SITE(site) (uwp_dl_site = UWP_DL_##site)
/* Event trace: the heartbeat hands out a small budget of lines now and then. */
extern int uwp_trace_budget;
extern int uwp_gpu_blits;
#define UWP_TRACE(...) do { \
    if (uwp_trace_budget > 0) { \
        uwp_trace_budget--; \
        uwp_log(__VA_ARGS__); \
    } \
} while (0)

#else /* !XEMU_UWP */

#define UWP_PROF_BEGIN(var) do { } while (0)
#define UWP_PROF_END(id, var) do { } while (0)
#define UWP_PROF_THREAD(name) do { } while (0)
#define UWP_PROF_SCOPE(id) do { } while (0)
#define UWP_PROF_COUNT(id) do { } while (0)
#define UWP_DL_SITE(site) do { } while (0)
#define UWP_TRACE(...) do { } while (0)

#endif /* XEMU_UWP */

#endif /* QEMU_XEMU_UWP_H */
