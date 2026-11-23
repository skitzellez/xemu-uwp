/*
 * xemu for Xbox Dev Mode: a "folder" disc driver, so an Xbox game that was unpacked to a folder (default.xbe and
 * the game's files beside it) plays as it is, without first being packed into an .iso.
 *
 * The folder is read once when the disc goes in: every file and folder in it is listed and an Xbox disc
 * (XDVDFS, the layout an .iso / XISO has) is laid out in memory around them - the volume descriptor at sector 32,
 * one directory table per folder, then each file's data, sector aligned. Only the tables live in memory; a read
 * of file data goes to that file on the drive. Nothing is copied or written.
 *
 * XDVDFS, as the Xbox kernel reads it:
 *   sector 32: "MICROSOFT*XBOX*MEDIA", root table sector (u32), root table size (u32), FILETIME, ..., the magic
 *              again at byte 2028.
 *   a directory table is a binary search tree of entries, each: left (u16), right (u16) - offsets in 4-byte units
 *   from the table's start, 0 = none - then start sector (u32), size (u32), attributes (u8), name length (u8),
 *   the name, padded to 4 bytes with 0xFF. An entry never crosses a 2048-byte sector; the rest of a sector is
 *   0xFF. Names are compared without case (A-Z = a-z), shorter first when one starts the other.
 *
 * The disc is "folder:<path>" or a json: description {"driver":"folder","filename":"<path>"}; ui/xemu.c makes it
 * from a disc path that is a folder or a default.xbe (xemu_uwp_disc_filename).
 *
 * Read-only. Reads run on QEMU's worker threads; files are opened on first use, a few kept open.
 *
 * Copyright (c) 2026 Skitzelles - part of Game Vault. Licensed like the rest of xemu (GPL-2.0-or-later).
 */

#ifndef UWP_FOLDER_TEST
#include "qemu/osdep.h"
#endif

#if defined(XEMU_UWP) || defined(UWP_FOLDER_TEST)

#ifndef UWP_FOLDER_TEST
#include "block/block_int.h"
#include "block/block-io.h"
#include "block/thread-pool.h"
#include "qemu/memalign.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qemu/option.h"
#include "qemu/cutils.h"
#include "qobject/qdict.h"
#include "qemu/xemu-uwp.h"
#else
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <windows.h>
#define uwp_log(...) (printf(__VA_ARGS__), printf("\n"))
#endif

#define FD_SECTOR 2048u
#define FD_MAX_DEPTH 24
#define FD_MAX_FILES 200000
#define FD_OPEN_SLOTS 16

typedef struct FdNode {
    char *name;           /* the name on the disc (Windows-1252 bytes, as Xbox names are) */
    uint8_t name_len;
    bool dir;
    uint64_t size;        /* a file's size; a folder: its table's size */
    uint32_t sector;      /* first sector of the data / table */
    wchar_t *path;        /* a file's full path on the drive */
    struct FdNode *kids;  /* a folder's contents, sorted as the disc wants them */
    int nkids;
    uint8_t *table;       /* a folder's table, built in memory */
} FdNode;

/* what each run of sectors of the disc is */
typedef struct FdExtent {
    uint32_t start, count;
    const uint8_t *mem;   /* bytes in memory (volume descriptor, a table), or NULL: */
    FdNode *file;         /* a file's data */
} FdExtent;

typedef struct FdOpen {
    FdNode *file;
    HANDLE h;
    uint64_t used;
} FdOpen;

typedef struct FdDisc {
    FdNode root;
    uint8_t vd[FD_SECTOR];
    FdExtent *ext;
    int next, cap;
    uint32_t sectors;
    int files, folders;
    uint64_t bytes;
    FdOpen open[FD_OPEN_SLOTS];
    uint64_t clock;
    int skipped;          /* files too big for a disc (4 GB) or with a name too long */
} FdDisc;

/* ---- the drive: listing folders, opening files (through the file broker on the console) ---- */

#ifndef UWP_FOLDER_TEST
typedef HANDLE (WINAPI *fd_FindFirstFileExFromAppW_fn)(LPCWSTR, FINDEX_INFO_LEVELS, LPVOID, FINDEX_SEARCH_OPS, LPVOID,
                                                        DWORD);
#endif

static HANDLE fd_find_first(const wchar_t *pattern, WIN32_FIND_DATAW *fd)
{
#ifndef UWP_FOLDER_TEST
    fd_FindFirstFileExFromAppW_fn fn = (fd_FindFirstFileExFromAppW_fn)xemu_uwp_kernelbase_proc("FindFirstFileExFromAppW");
    if (fn) {
        return fn(pattern, FindExInfoBasic, fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    }
#endif
    return FindFirstFileExW(pattern, FindExInfoBasic, fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
}

static HANDLE fd_open_file(const wchar_t *path)
{
#ifndef UWP_FOLDER_TEST
    return xemu_uwp_create_file_w(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_EXISTING, 0);
#else
    return CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
#endif
}

static bool fd_is_folder(const wchar_t *path)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
#ifndef UWP_FOLDER_TEST
    typedef BOOL (WINAPI *attr_fn)(LPCWSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
    attr_fn fn = (attr_fn)xemu_uwp_kernelbase_proc("GetFileAttributesExFromAppW");
    if (fn) {
        return fn(path, GetFileExInfoStandard, &a) && (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
    }
#endif
    return GetFileAttributesExW(path, GetFileExInfoStandard, &a) && (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
}

/* ---- names and order ---- */

static int fd_upper(int c)
{
    return c >= 'a' && c <= 'z' ? c - 32 : c;
}

static int fd_compare(const FdNode *a, const FdNode *b)
{
    int n = a->name_len < b->name_len ? a->name_len : b->name_len;
    for (int i = 0; i < n; i++) {
        int x = fd_upper((uint8_t)a->name[i]), y = fd_upper((uint8_t)b->name[i]);
        if (x != y) {
            return x - y;
        }
    }
    return a->name_len - b->name_len;
}

static int fd_compare_q(const void *a, const void *b)
{
    return fd_compare(a, b);
}

/* ---- reading the folder ---- */

static wchar_t *fd_join(const wchar_t *dir, const wchar_t *name)
{
    size_t a = wcslen(dir), b = wcslen(name);
    wchar_t *p = malloc((a + b + 2) * sizeof(wchar_t));
    memcpy(p, dir, a * sizeof(wchar_t));
    size_t at = a;
    if (at && dir[at - 1] != L'\\' && dir[at - 1] != L'/') {
        p[at++] = L'\\';
    }
    memcpy(p + at, name, (b + 1) * sizeof(wchar_t));
    return p;
}

static bool fd_scan(FdDisc *d, FdNode *node, const wchar_t *dir, int depth)
{
    if (depth > FD_MAX_DEPTH) {
        return true;
    }
    wchar_t *pattern = fd_join(dir, L"*");
    WIN32_FIND_DATAW fd;
    HANDLE h = fd_find_first(pattern, &fd);
    free(pattern);
    if (h == INVALID_HANDLE_VALUE) {
        return depth > 0;   /* the game's own folder must open; an inner one that does not is left empty */
    }
    int cap = 16;
    node->kids = calloc(cap, sizeof(FdNode));
    do {
        const wchar_t *n = fd.cFileName;
        if (!wcscmp(n, L".") || !wcscmp(n, L"..")) {
            continue;
        }
        bool dir_entry = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        uint64_t size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        char name[256];
        BOOL lossy = FALSE;
        int len = WideCharToMultiByte(1252, 0, n, -1, name, sizeof(name), "_", &lossy);
        if (len <= 1 || len > 256 || (!dir_entry && size > 0xFFFFFFFFull)) {
            d->skipped++;
            continue;
        }
        if (d->files + d->folders >= FD_MAX_FILES) {
            break;
        }
        if (node->nkids == cap) {
            cap *= 2;
            node->kids = realloc(node->kids, cap * sizeof(FdNode));
            memset(node->kids + node->nkids, 0, (cap - node->nkids) * sizeof(FdNode));
        }
        FdNode *k = &node->kids[node->nkids++];
        k->name_len = (uint8_t)(len - 1);
        k->name = malloc(len);
        memcpy(k->name, name, len);
        k->dir = dir_entry;
        k->path = fd_join(dir, n);
        if (dir_entry) {
            d->folders++;
        } else {
            k->size = size;
            d->files++;
            d->bytes += size;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    qsort(node->kids, node->nkids, sizeof(FdNode), fd_compare_q);
    /* two names the disc cannot tell apart (only case differs): the second one is left out */
    for (int i = 1; i < node->nkids; i++) {
        if (!fd_compare(&node->kids[i - 1], &node->kids[i])) {
            free(node->kids[i].name);
            free(node->kids[i].path);
            memmove(&node->kids[i], &node->kids[i + 1], (node->nkids - i - 1) * sizeof(FdNode));
            node->nkids--;
            i--;
            d->skipped++;
        }
    }
    for (int i = 0; i < node->nkids; i++) {
        if (node->kids[i].dir && !fd_scan(d, &node->kids[i], node->kids[i].path, depth + 1)) {
            return false;
        }
    }
    return true;
}

/* ---- laying out the disc ---- */

static uint32_t fd_entry_size(const FdNode *k)
{
    return (14 + k->name_len + 3) & ~3u;
}

/* where each entry goes in the table, in the order the tree is written (root of the subtree first) */
typedef struct FdPlace {
    uint32_t at;
    int left, right;   /* index into kids, -1 none */
} FdPlace;

static int fd_place(const FdNode *dir, FdPlace *place, int lo, int hi, uint32_t *pos)
{
    if (lo > hi) {
        return -1;
    }
    int mid = (lo + hi) / 2;
    uint32_t sz = fd_entry_size(&dir->kids[mid]);
    if ((*pos % FD_SECTOR) + sz > FD_SECTOR) {
        *pos = (*pos + FD_SECTOR - 1) / FD_SECTOR * FD_SECTOR;
    }
    place[mid].at = *pos;
    *pos += sz;
    place[mid].left = fd_place(dir, place, lo, mid - 1, pos);
    place[mid].right = fd_place(dir, place, mid + 1, hi, pos);
    return mid;
}

static void fd_put16(uint8_t *p, uint16_t v)
{
    p[0] = v;
    p[1] = v >> 8;
}

static void fd_put32(uint8_t *p, uint32_t v)
{
    p[0] = v;
    p[1] = v >> 8;
    p[2] = v >> 16;
    p[3] = v >> 24;
}

/* sizes of every table, first pass (the places do not depend on sectors) */
static void fd_size_tables(FdNode *dir)
{
    if (!dir->nkids) {
        dir->size = FD_SECTOR;   /* an empty folder: one sector of 0xFF */
        return;
    }
    FdPlace *place = calloc(dir->nkids, sizeof(FdPlace));
    uint32_t pos = 0;
    fd_place(dir, place, 0, dir->nkids - 1, &pos);
    free(place);
    dir->size = (pos + FD_SECTOR - 1) / FD_SECTOR * FD_SECTOR;
    for (int i = 0; i < dir->nkids; i++) {
        if (dir->kids[i].dir) {
            fd_size_tables(&dir->kids[i]);
        }
    }
}

static uint32_t fd_sectors_of(uint64_t bytes)
{
    return (uint32_t)((bytes + FD_SECTOR - 1) / FD_SECTOR);
}

static void fd_add_extent(FdDisc *d, uint32_t start, uint32_t count, const uint8_t *mem, FdNode *file)
{
    if (!count) {
        return;
    }
    if (d->next == d->cap) {
        d->cap = d->cap ? d->cap * 2 : 256;
        d->ext = realloc(d->ext, d->cap * sizeof(FdExtent));
    }
    d->ext[d->next++] = (FdExtent){ start, count, mem, file };
}

/* tables first, folder by folder (breadth first, so the root and the game's main folders sit together) */
static void fd_assign_tables(FdDisc *d, uint32_t *next)
{
    int cap = 64, head = 0, tail = 0;
    FdNode **queue = malloc(cap * sizeof(FdNode *));
    queue[tail++] = &d->root;
    while (head < tail) {
        FdNode *dir = queue[head++];
        dir->sector = *next;
        *next += fd_sectors_of(dir->size);
        for (int i = 0; i < dir->nkids; i++) {
            if (dir->kids[i].dir) {
                if (tail == cap) {
                    cap *= 2;
                    queue = realloc(queue, cap * sizeof(FdNode *));
                }
                queue[tail++] = &dir->kids[i];
            }
        }
    }
    free(queue);
}

static void fd_assign_files(FdDisc *d, FdNode *dir, uint32_t *next)
{
    for (int i = 0; i < dir->nkids; i++) {
        FdNode *k = &dir->kids[i];
        if (!k->dir) {
            k->sector = k->size ? *next : 0;
            fd_add_extent(d, k->sector, fd_sectors_of(k->size), NULL, k);
            *next += fd_sectors_of(k->size);
        }
    }
    for (int i = 0; i < dir->nkids; i++) {
        if (dir->kids[i].dir) {
            fd_assign_files(d, &dir->kids[i], next);
        }
    }
}

static void fd_write_tables(FdDisc *d, FdNode *dir)
{
    dir->table = malloc(dir->size);
    memset(dir->table, 0xFF, dir->size);
    if (dir->nkids) {
        FdPlace *place = calloc(dir->nkids, sizeof(FdPlace));
        uint32_t pos = 0;
        fd_place(dir, place, 0, dir->nkids - 1, &pos);
        for (int i = 0; i < dir->nkids; i++) {
            FdNode *k = &dir->kids[i];
            uint8_t *e = dir->table + place[i].at;
            fd_put16(e, place[i].left < 0 ? 0 : place[place[i].left].at / 4);
            fd_put16(e + 2, place[i].right < 0 ? 0 : place[place[i].right].at / 4);
            fd_put32(e + 4, k->sector);
            fd_put32(e + 8, (uint32_t)k->size);
            e[12] = k->dir ? 0x10 : 0x20;   /* directory / archive */
            e[13] = k->name_len;
            memcpy(e + 14, k->name, k->name_len);
        }
        free(place);
    }
    fd_add_extent(d, dir->sector, fd_sectors_of(dir->size), dir->table, NULL);
    for (int i = 0; i < dir->nkids; i++) {
        if (dir->kids[i].dir) {
            fd_write_tables(d, &dir->kids[i]);
        }
    }
}

static int fd_extent_order(const void *a, const void *b)
{
    const FdExtent *x = a, *y = b;
    return x->start < y->start ? -1 : x->start > y->start;
}

static void fd_free_node(FdNode *n)
{
    for (int i = 0; i < n->nkids; i++) {
        fd_free_node(&n->kids[i]);
    }
    free(n->kids);
    free(n->name);
    free(n->path);
    free(n->table);
}

static void fd_free(FdDisc *d)
{
    for (int i = 0; i < FD_OPEN_SLOTS; i++) {
        if (d->open[i].file) {
            CloseHandle(d->open[i].h);
        }
    }
    fd_free_node(&d->root);
    free(d->ext);
    memset(d, 0, sizeof(*d));
}

/* Lists the folder and lays the disc out. false: the folder could not be read (or has no default.xbe). */
static bool fd_build(FdDisc *d, const wchar_t *folder, char *why, size_t why_len)
{
    memset(d, 0, sizeof(*d));
    d->root.dir = true;
    if (!fd_scan(d, &d->root, folder, 0)) {
        snprintf(why, why_len, "the folder cannot be opened (error %lu)", GetLastError());
        return false;
    }
    bool xbe = false;
    for (int i = 0; i < d->root.nkids; i++) {
        if (!d->root.kids[i].dir && d->root.kids[i].name_len == 11 && !_strnicmp(d->root.kids[i].name, "default.xbe", 11)) {
            xbe = true;
        }
    }
    if (!xbe) {
        snprintf(why, why_len, "there is no default.xbe in the folder");
        return false;
    }
    fd_size_tables(&d->root);
    uint32_t next = 33;
    fd_assign_tables(d, &next);
    fd_assign_files(d, &d->root, &next);
    d->sectors = next;
    fd_write_tables(d, &d->root);

    memcpy(d->vd, "MICROSOFT*XBOX*MEDIA", 20);
    fd_put32(d->vd + 20, d->root.sector);
    fd_put32(d->vd + 24, (uint32_t)d->root.size);
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    fd_put32(d->vd + 28, now.dwLowDateTime);
    fd_put32(d->vd + 32, now.dwHighDateTime);
    memcpy(d->vd + 0x7EC, "MICROSOFT*XBOX*MEDIA", 20);
    fd_add_extent(d, 32, 1, d->vd, NULL);
    qsort(d->ext, d->next, sizeof(FdExtent), fd_extent_order);
    return true;
}

/* ---- reading the disc ---- */

static HANDLE fd_handle(FdDisc *d, FdNode *file)
{
    int lru = 0;
    for (int i = 0; i < FD_OPEN_SLOTS; i++) {
        if (d->open[i].file == file) {
            d->open[i].used = ++d->clock;
            return d->open[i].h;
        }
        if (d->open[i].used < d->open[lru].used) {
            lru = i;
        }
    }
    HANDLE h = fd_open_file(file->path);
    if (h == INVALID_HANDLE_VALUE) {
        return h;
    }
    if (d->open[lru].file) {
        CloseHandle(d->open[lru].h);
    }
    d->open[lru] = (FdOpen){ file, h, ++d->clock };
    return h;
}

/* bytes at offset of the disc into buf; 0, or -EIO when a file could not be read */
static int fd_read(FdDisc *d, uint64_t offset, uint8_t *buf, uint64_t bytes)
{
    while (bytes) {
        uint32_t sector = (uint32_t)(offset / FD_SECTOR);
        /* the last extent that starts at or before this sector */
        int lo = 0, hi = d->next - 1, at = -1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            if (d->ext[mid].start <= sector) {
                at = mid;
                lo = mid + 1;
            } else {
                hi = mid - 1;
            }
        }
        uint64_t chunk;
        if (at < 0 || sector >= d->ext[at].start + d->ext[at].count) {
            /* between extents (the first 32 sectors): zeros up to the next one */
            uint64_t end = (at + 1 < d->next) ? (uint64_t)d->ext[at + 1].start * FD_SECTOR : (uint64_t)d->sectors * FD_SECTOR;
            chunk = end > offset ? end - offset : bytes;
            if (chunk > bytes) {
                chunk = bytes;
            }
            memset(buf, 0, chunk);
        } else {
            FdExtent *e = &d->ext[at];
            uint64_t base = (uint64_t)e->start * FD_SECTOR, in = offset - base;
            uint64_t left = (uint64_t)e->count * FD_SECTOR - in;
            chunk = bytes < left ? bytes : left;
            if (e->mem) {
                memcpy(buf, e->mem + in, chunk);
            } else {
                uint64_t real = in < e->file->size ? e->file->size - in : 0;   /* past the end: zeros */
                uint64_t want = chunk < real ? chunk : real;
                if (want) {
                    HANDLE h = fd_handle(d, e->file);
                    OVERLAPPED ov = { 0 };
                    ov.Offset = (DWORD)in;
                    ov.OffsetHigh = (DWORD)(in >> 32);
                    DWORD got = 0;
                    if (h == INVALID_HANDLE_VALUE || !ReadFile(h, buf, (DWORD)want, &got, &ov) || got != want) {
                        uwp_log("folder disc: could not read %ls (error %lu)", e->file->path, GetLastError());
                        return -EIO;
                    }
                }
                if (want < chunk) {
                    memset(buf + want, 0, chunk - want);
                }
            }
        }
        buf += chunk;
        offset += chunk;
        bytes -= chunk;
    }
    return 0;
}

#ifndef UWP_FOLDER_TEST

/* ---- the block driver ---- */

typedef struct BDRVFolderState {
    FdDisc disc;
    char *folder;
    QemuMutex lock;
    uint64_t reads, read_bytes;
} BDRVFolderState;

static QemuOptsList folder_runtime_opts = {
    .name = "folder",
    .head = QTAILQ_HEAD_INITIALIZER(folder_runtime_opts.head),
    .desc = {
        { .name = "filename", .type = QEMU_OPT_STRING, .help = "the game's folder (default.xbe in it)" },
        { /* end of list */ }
    },
};

static void folder_parse_filename(const char *filename, QDict *options, Error **errp)
{
    strstart(filename, "folder:", &filename);
    qdict_put_str(options, "filename", filename);
}

static int folder_open(BlockDriverState *bs, QDict *options, int flags, Error **errp)
{
    BDRVFolderState *s = bs->opaque;
    QemuOpts *opts = qemu_opts_create(&folder_runtime_opts, NULL, 0, &error_abort);
    int ret = 0;
    if (!qemu_opts_absorb_qdict(opts, options, errp)) {
        ret = -EINVAL;
        goto out;
    }
    const char *folder = qemu_opt_get(opts, "filename");
    if (!folder || !folder[0]) {
        error_setg(errp, "folder disc: no folder given");
        ret = -EINVAL;
        goto out;
    }
    ret = bdrv_apply_auto_read_only(bs, "a folder disc is read-only", errp);
    if (ret < 0) {
        goto out;
    }
    wchar_t *w = g_utf8_to_utf16(folder, -1, NULL, NULL, NULL);
    if (!w) {
        error_setg(errp, "folder disc: bad folder name");
        ret = -EINVAL;
        goto out;
    }
    for (wchar_t *c = w; *c; c++) {
        if (*c == L'/') {
            *c = L'\\';
        }
    }
    uint64_t t0 = g_get_monotonic_time();
    char why[160] = "";
    bool ok = fd_build(&s->disc, w, why, sizeof(why));
    g_free(w);
    if (!ok) {
        uwp_log("folder disc: %s: %s", folder, why);
        error_setg(errp, "folder disc: %s", why);
        fd_free(&s->disc);
        ret = -EIO;
        goto out;
    }
    s->folder = g_strdup(folder);
    qemu_mutex_init(&s->lock);
    uwp_log("folder disc: %s as a disc: %d files in %d folders, %llu MB, read in %llu ms%s",
            folder, s->disc.files, s->disc.folders, (unsigned long long)(s->disc.bytes >> 20),
            (unsigned long long)((g_get_monotonic_time() - t0) / 1000),
            s->disc.skipped ? " (some names left out)" : "");
    if (s->disc.skipped) {
        uwp_log("folder disc: %d left out: files over 4 GB, names too long or the same as another apart from case",
                s->disc.skipped);
    }
out:
    qemu_opts_del(opts);
    return ret;
}

static void folder_close(BlockDriverState *bs)
{
    BDRVFolderState *s = bs->opaque;
    if (s->folder) {
        uwp_log("folder disc: %s closed after %llu MB in %llu reads", s->folder,
                (unsigned long long)(s->read_bytes >> 20), (unsigned long long)s->reads);
        qemu_mutex_destroy(&s->lock);
    }
    fd_free(&s->disc);
    g_free(s->folder);
}

typedef struct FolderRead {
    BDRVFolderState *s;
    int64_t offset, bytes;
    uint8_t *buf;
} FolderRead;

static int folder_read_worker(void *opaque)
{
    FolderRead *r = opaque;
    qemu_mutex_lock(&r->s->lock);
    int ret = fd_read(&r->s->disc, r->offset, r->buf, r->bytes);
    r->s->reads++;
    r->s->read_bytes += r->bytes;
    qemu_mutex_unlock(&r->s->lock);
    return ret;
}

static int coroutine_fn folder_co_preadv(BlockDriverState *bs, int64_t offset, int64_t bytes, QEMUIOVector *qiov,
                                         BdrvRequestFlags flags)
{
    BDRVFolderState *s = bs->opaque;
    FolderRead r = { s, offset, bytes, qemu_try_blockalign(bs, bytes) };
    if (!r.buf) {
        return -ENOMEM;
    }
    int ret = thread_pool_submit_co(folder_read_worker, &r);
    if (ret == 0) {
        qemu_iovec_from_buf(qiov, 0, r.buf, bytes);
    }
    qemu_vfree(r.buf);
    return ret;
}

static int coroutine_fn folder_co_pwritev(BlockDriverState *bs, int64_t offset, int64_t bytes, QEMUIOVector *qiov,
                                          BdrvRequestFlags flags)
{
    return -EROFS;
}

static int64_t coroutine_fn folder_co_getlength(BlockDriverState *bs)
{
    BDRVFolderState *s = bs->opaque;
    return (int64_t)s->disc.sectors * FD_SECTOR;
}

static BlockDriver bdrv_uwp_folder = {
    .format_name         = "folder",
    .protocol_name       = "folder",
    .instance_size       = sizeof(BDRVFolderState),
    .bdrv_parse_filename = folder_parse_filename,
    .bdrv_open           = folder_open,
    .bdrv_close          = folder_close,
    .bdrv_co_preadv      = folder_co_preadv,
    .bdrv_co_pwritev     = folder_co_pwritev,
    .bdrv_co_getlength   = folder_co_getlength,
};

static void bdrv_uwp_folder_init(void)
{
    bdrv_register(&bdrv_uwp_folder);
}

block_init(bdrv_uwp_folder_init);

/* ui/xemu.c: is this disc path a game folder (or its default.xbe)? Fills the folder to open. */
bool xemu_uwp_folder_disc(const char *path, char **folder);
bool xemu_uwp_folder_disc(const char *path, char **folder)
{
    size_t n = strlen(path);
    if (n > 12 && !g_ascii_strcasecmp(path + n - 11, "default.xbe") && (path[n - 12] == '\\' || path[n - 12] == '/')) {
        *folder = g_strndup(path, n - 12);
        return true;
    }
    wchar_t *w = g_utf8_to_utf16(path, -1, NULL, NULL, NULL);
    bool dir = w && fd_is_folder(w);
    g_free(w);
    if (dir) {
        *folder = g_strdup(path);
        while (n > 3 && ((*folder)[n - 1] == '\\' || (*folder)[n - 1] == '/')) {
            (*folder)[--n] = 0;
        }
    }
    return dir;
}

#else  /* UWP_FOLDER_TEST: uwp-folder.exe <folder> <out.iso> writes the disc it would show, to check on a PC */

int main(int argc, char **argv)
{
    if (argc < 3) {
        printf("uwp-folder <folder> <out.iso>\n");
        return 2;
    }
    wchar_t folder[1024];
    MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, folder, 1024);
    FdDisc d;
    char why[160];
    DWORD t0 = GetTickCount();
    if (!fd_build(&d, folder, why, sizeof why)) {
        printf("failed: %s\n", why);
        return 1;
    }
    printf("%d files, %d folders, %llu MB, %u sectors, built in %lu ms, %d skipped\n", d.files, d.folders,
           (unsigned long long)(d.bytes >> 20), d.sectors, GetTickCount() - t0, d.skipped);
    FILE *f = fopen(argv[2], "wb");
    if (!f) {
        return 1;
    }
    static uint8_t buf[1 << 20];
    uint64_t total = (uint64_t)d.sectors * FD_SECTOR;
    for (uint64_t at = 0; at < total; at += sizeof buf) {
        uint64_t n = total - at < sizeof buf ? total - at : sizeof buf;
        /* odd sizes and offsets on purpose, as the block layer may ask */
        uint64_t half = n / 3 + 1;
        if (fd_read(&d, at, buf, half) || fd_read(&d, at + half, buf + half, n - half)) {
            printf("read failed at %llu\n", (unsigned long long)at);
            return 1;
        }
        fwrite(buf, 1, n, f);
    }
    fclose(f);
    fd_free(&d);
    printf("written\n");
    return 0;
}

#endif
#endif
