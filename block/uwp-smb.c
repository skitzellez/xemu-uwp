/*
 * Copyright (c) 2026 Skitzelles - part of xemu for Xbox Dev Mode (github.com/skitzellez/xemu-uwp). GPL-2.0-or-later.
 *
 * xemu for Xbox Dev Mode: an "smb" disc driver, so an Xbox game plays straight from a PC's shared folder or a NAS
 * (Game Vault's network games). Apps on the Xbox cannot open \\PC\share paths (the console answers error 65), so
 * this speaks SMB2/3 itself through libsmb2 (smb2.dll, LGPL-2.1, the same build Game Vault uses).
 *
 * The disc is "smb://<server>/<share>/<path in the share>". The sign-in comes from Game Vault: link value
 * netcred=<file>, a file in the folder only Game Vault and its emulators can open, holding one line per share
 * "server<tab>share<tab>user<tab>password". It is read once, kept in memory only, written over and deleted.
 *
 * Read-only. Reads run on QEMU's worker threads (the emulated Xbox never waits on the network in its own thread)
 * through a read-ahead cache of 1 MB pieces, so a disc read of a few KB does not cost a network round trip each;
 * a dropped connection (a PC that slept, Wi-Fi that blinked) is opened again by itself, for up to ~10 s.
 *
 * Copyright (c) 2026 Skitzelles - part of Game Vault. Licensed like the rest of xemu (GPL-2.0-or-later).
 */

#include "qemu/osdep.h"

#ifdef XEMU_UWP

#include "block/block_int.h"
#include "block/block-io.h"
#include "block/thread-pool.h"
#include "qemu/memalign.h"
#include <glib/gstdio.h>
#include "qapi/error.h"
#include "qemu/module.h"
#include "qemu/option.h"
#include "qemu/cutils.h"
#include "qobject/qdict.h"
#include "qemu/xemu-uwp.h"

#include <smb2/smb2.h>
#include <smb2/libsmb2.h>

void xemu_uwp_smb_set_credentials_file(const char *path);
bool xemu_uwp_smb_peek(const char *url, uint64_t offset, void *buf, uint32_t len);

#define SMB_CHUNK (1u << 20)   /* read-ahead piece */
#define SMB_SLOTS 16           /* 16 MB of them */

/* ---- the sign-in Game Vault handed over ---------------------------------------------------------------------- */

typedef struct SmbCred {
    char *server, *share, *user, *password;
} SmbCred;
static SmbCred *smb_creds;
static int smb_ncreds;
static QemuMutex smb_cred_lock;
static bool smb_cred_lock_ready;

static void smb_cred_lock_init(void)
{
    if (!smb_cred_lock_ready) {
        qemu_mutex_init(&smb_cred_lock);
        /* the disc is looked at (full dump?) before QEMU starts the network itself */
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        smb_cred_lock_ready = true;
    }
}

void xemu_uwp_smb_set_credentials_file(const char *path)
{
    gchar *text = NULL;
    gsize len = 0;
    smb_cred_lock_init();
    if (!g_file_get_contents(path, &text, &len, NULL)) {
        uwp_log("network: the sign-in from Game Vault could not be read");
        return;
    }
    /* gone from the disk at once: written over, then deleted */
    gchar *blank = g_strnfill(len, ' ');
    g_file_set_contents(path, blank, len, NULL);
    g_free(blank);
    g_unlink(path);
    qemu_mutex_lock(&smb_cred_lock);
    gchar **lines = g_strsplit(text, "\n", -1);
    for (int i = 0; lines[i]; i++) {
        g_strchomp(lines[i]);
        gchar **f = g_strsplit(lines[i], "\t", 4);
        if (g_strv_length(f) == 4 && f[0][0] && f[1][0]) {
            smb_creds = g_renew(SmbCred, smb_creds, smb_ncreds + 1);
            smb_creds[smb_ncreds++] = (SmbCred){ g_strdup(f[0]), g_strdup(f[1]), g_strdup(f[2]), g_strdup(f[3]) };
        }
        memset(lines[i], ' ', strlen(lines[i]));
        for (int k = 0; f[k]; k++) {
            memset(f[k], ' ', strlen(f[k]));
        }
        g_strfreev(f);
    }
    g_strfreev(lines);
    memset(text, ' ', len);
    g_free(text);
    qemu_mutex_unlock(&smb_cred_lock);
    uwp_log("network: sign-in for %d share(s) from Game Vault", smb_ncreds);
}

/* "smb://server/share/a/b.iso" -> server, share, "a\b.iso" (libsmb2 sends the path as it is: Windows separators) */
static bool smb_split(const char *url, char **server, char **share, char **path)
{
    if (!g_str_has_prefix(url, "smb://")) {
        return false;
    }
    const char *s = url + 6, *a = strchr(s, '/');
    if (!a || a == s) {
        return false;
    }
    const char *b = strchr(a + 1, '/');
    if (!b || b == a + 1 || !b[1]) {
        return false;
    }
    *server = g_strndup(s, a - s);
    *share = g_strndup(a + 1, b - a - 1);
    *path = g_strdup(b + 1);
    for (char *c = *path; *c; c++) {
        if (*c == '/') {
            *c = '\\';
        }
    }
    return true;
}

static struct smb2_context *smb_connect(const char *server, const char *share, Error **errp)
{
    const char *user = "Guest", *password = "";
    char *domain = NULL;
    smb_cred_lock_init();
    qemu_mutex_lock(&smb_cred_lock);
    for (int i = 0; i < smb_ncreds; i++) {
        if (!g_ascii_strcasecmp(smb_creds[i].server, server) && !g_ascii_strcasecmp(smb_creds[i].share, share)) {
            user = smb_creds[i].user[0] ? smb_creds[i].user : "Guest";
            password = smb_creds[i].password;
            break;
        }
    }
    char *u = g_strdup(user), *p = g_strdup(password);
    qemu_mutex_unlock(&smb_cred_lock);
    char *slash = strchr(u, '\\');
    if (slash) {   /* DOMAIN\user */
        *slash = 0;
        domain = u;
        u = g_strdup(slash + 1);
    }
    struct smb2_context *c = smb2_init_context();
    if (!c) {
        error_setg(errp, "network: out of memory");
        goto out;
    }
    smb2_set_timeout(c, 15);
    smb2_set_security_mode(c, SMB2_NEGOTIATE_SIGNING_ENABLED);
    if (domain) {
        smb2_set_domain(c, domain);
    }
    smb2_set_password(c, p);
    if (smb2_connect_share(c, server, share, u) < 0) {
        error_setg(errp, "network: %s\\%s: %s", server, share, smb2_get_error(c));
        smb2_destroy_context(c);
        c = NULL;
    }
out:
    memset(p, 0, strlen(p));
    g_free(p);
    g_free(u);
    g_free(domain);
    return c;
}

/* ---- the driver ---------------------------------------------------------------------------------------------- */

typedef struct SmbSlot {
    int64_t chunk;   /* -1: empty */
    uint32_t len;
    uint64_t used;
    uint8_t *buf;
} SmbSlot;

typedef struct BDRVSmbState {
    QemuMutex lock;
    char *url, *server, *share, *path;
    struct smb2_context *ctx;
    struct smb2fh *fh;
    uint64_t length, tick;
    uint32_t max_read;
    SmbSlot slot[SMB_SLOTS];
    uint64_t net_bytes, net_reads;
} BDRVSmbState;

static void smb_drop(BDRVSmbState *s)
{
    if (s->ctx) {
        if (s->fh) {
            smb2_close(s->ctx, s->fh);
        }
        smb2_disconnect_share(s->ctx);
        smb2_destroy_context(s->ctx);
    }
    s->ctx = NULL;
    s->fh = NULL;
}

static int smb_open_file(BDRVSmbState *s, Error **errp)
{
    s->ctx = smb_connect(s->server, s->share, errp);
    if (!s->ctx) {
        return -EIO;
    }
    s->fh = smb2_open(s->ctx, s->path, O_RDONLY);
    if (!s->fh) {
        error_setg(errp, "network: %s could not be opened: %s", s->url, smb2_get_error(s->ctx));
        smb_drop(s);
        return -ENOENT;
    }
    struct smb2_stat_64 st;
    if (smb2_fstat(s->ctx, s->fh, &st) < 0) {
        error_setg(errp, "network: %s: %s", s->url, smb2_get_error(s->ctx));
        smb_drop(s);
        return -EIO;
    }
    s->length = st.smb2_size;
    s->max_read = smb2_get_max_read_size(s->ctx);
    if (s->max_read < 65536) {
        s->max_read = 65536;
    }
    return 0;
}

/* one piece into a slot, from the network (lock held). A failed read is tried again, with the connection opened
 * afresh, for about 10 s (a PC busy for a moment, Wi-Fi that blinked) before the game is told the disc can't be
 * read - after 2 quick tries the game showed its "clean the disc" screen while the share was fine again. */
#define SMB_RETRY_US (10 * G_USEC_PER_SEC)
static int smb_fetch(BDRVSmbState *s, SmbSlot *slot, int64_t chunk)
{
    uint64_t off = (uint64_t)chunk * SMB_CHUNK;
    uint32_t want = off + SMB_CHUNK <= s->length ? SMB_CHUNK : (uint32_t)(s->length - off);
    int64_t start = 0;
    int64_t wait_us = 250 * 1000;
    for (int attempt = 0;; attempt++) {
        if (attempt >= 2) {   /* the first retry at once, then waits of 0.25, 0.5, 1, 2, 2 ... s */
            if (g_get_monotonic_time() - start + wait_us > SMB_RETRY_US) {
                break;
            }
            g_usleep(wait_us);
            wait_us = MIN(wait_us * 2, 2 * G_USEC_PER_SEC);
        }
        if (!s->ctx) {
            Error *err = NULL;
            if (smb_open_file(s, &err) < 0) {
                uwp_log("%s", error_get_pretty(err));
                error_free(err);
                if (!start) {
                    start = g_get_monotonic_time();
                }
                continue;
            }
            uwp_log("network: connection opened again (%s)", s->server);
        }
        uint32_t done = 0;
        while (done < want) {
            uint32_t n = MIN(want - done, s->max_read);
            int got = smb2_pread(s->ctx, s->fh, slot->buf + done, n, off + done);
            if (got <= 0) {
                break;
            }
            done += got;
        }
        if (done == want) {
            slot->chunk = chunk;
            slot->len = want;
            s->net_bytes += want;
            s->net_reads++;
            if (start) {
                uwp_log("network: read at %llu MB recovered after %lld ms (%d tries)",
                        (unsigned long long)(off >> 20), (long long)((g_get_monotonic_time() - start) / 1000),
                        attempt + 1);
            }
            return 0;
        }
        if (!start) {
            start = g_get_monotonic_time();
        }
        uwp_log("network: read at %llu MB failed (%s), trying again", (unsigned long long)(off >> 20),
                s->ctx ? smb2_get_error(s->ctx) : "no connection");
        smb_drop(s);
    }
    uwp_log("network: read at %llu MB gave up after %lld ms - the game is told the disc can't be read",
            (unsigned long long)(off >> 20), (long long)((g_get_monotonic_time() - start) / 1000));
    slot->chunk = -1;
    return -EIO;
}

typedef struct SmbRead {
    BDRVSmbState *s;
    uint64_t offset;
    uint64_t bytes;
    uint8_t *buf;
} SmbRead;

static int smb_read_worker(void *opaque)
{
    SmbRead *r = opaque;
    BDRVSmbState *s = r->s;
    int ret = 0;
    qemu_mutex_lock(&s->lock);
    uint64_t pos = r->offset, end = r->offset + r->bytes;
    while (pos < end) {
        if (pos >= s->length) {   /* past the end of the image: zeros, as a disc reads */
            memset(r->buf + (pos - r->offset), 0, end - pos);
            break;
        }
        int64_t chunk = pos / SMB_CHUNK;
        SmbSlot *hit = NULL, *old = &s->slot[0];
        for (int i = 0; i < SMB_SLOTS; i++) {
            if (s->slot[i].chunk == chunk) {
                hit = &s->slot[i];
                break;
            }
            if (s->slot[i].used < old->used) {
                old = &s->slot[i];
            }
        }
        if (!hit) {
            if (smb_fetch(s, old, chunk) < 0) {
                ret = -EIO;
                break;
            }
            hit = old;
        }
        hit->used = ++s->tick;
        uint64_t within = pos - (uint64_t)chunk * SMB_CHUNK;
        uint64_t n = MIN(end - pos, hit->len - within);
        if (!n) {
            ret = -EIO;
            break;
        }
        memcpy(r->buf + (pos - r->offset), hit->buf + within, n);
        pos += n;
    }
    qemu_mutex_unlock(&s->lock);
    return ret;
}

static QemuOptsList smb_runtime_opts = {
    .name = "smb",
    .head = QTAILQ_HEAD_INITIALIZER(smb_runtime_opts.head),
    .desc = {
        { .name = "url", .type = QEMU_OPT_STRING, .help = "smb://server/share/path" },
        { /* end of list */ }
    },
};

static void smb_parse_filename(const char *filename, QDict *options, Error **errp)
{
    if (qdict_haskey(options, "url")) {
        error_setg(errp, "url and a filename cannot both be given");
        return;
    }
    qdict_put_str(options, "url", filename);
}

static int smb_file_open(BlockDriverState *bs, QDict *options, int flags, Error **errp)
{
    BDRVSmbState *s = bs->opaque;
    QemuOpts *opts = qemu_opts_create(&smb_runtime_opts, NULL, 0, &error_abort);
    int ret;
    if (!qemu_opts_absorb_qdict(opts, options, errp)) {
        ret = -EINVAL;
        goto out;
    }
    const char *url = qemu_opt_get(opts, "url");
    if (!url || !smb_split(url, &s->server, &s->share, &s->path)) {
        error_setg(errp, "network: not a share path: %s", url ? url : "(none)");
        ret = -EINVAL;
        goto out;
    }
    ret = bdrv_apply_auto_read_only(bs, "a network disc is read-only", errp);
    if (ret < 0) {
        goto out;
    }
    s->url = g_strdup(url);
    qemu_mutex_init(&s->lock);
    for (int i = 0; i < SMB_SLOTS; i++) {
        s->slot[i].chunk = -1;
        s->slot[i].buf = g_malloc(SMB_CHUNK);
    }
    uint64_t t0 = g_get_monotonic_time();
    ret = smb_open_file(s, errp);
    if (ret == 0) {
        uwp_log("network: playing %s straight from the network (%llu MB, connected in %llu ms, reads of up to %u KB)",
                s->url, (unsigned long long)(s->length >> 20),
                (unsigned long long)((g_get_monotonic_time() - t0) / 1000), s->max_read >> 10);
    }
out:
    qemu_opts_del(opts);
    return ret;
}

static void smb_file_close(BlockDriverState *bs)
{
    BDRVSmbState *s = bs->opaque;
    if (s->url) {
        uwp_log("network: %s closed after %llu MB in %llu reads", s->url,
                (unsigned long long)(s->net_bytes >> 20), (unsigned long long)s->net_reads);
    }
    smb_drop(s);
    for (int i = 0; i < SMB_SLOTS; i++) {
        g_free(s->slot[i].buf);
    }
    if (s->url) {
        qemu_mutex_destroy(&s->lock);
    }
    g_free(s->url);
    g_free(s->server);
    g_free(s->share);
    g_free(s->path);
}

static int coroutine_fn smb_co_preadv(BlockDriverState *bs, int64_t offset, int64_t bytes, QEMUIOVector *qiov,
                                      BdrvRequestFlags flags)
{
    BDRVSmbState *s = bs->opaque;
    SmbRead r = { s, offset, bytes, qemu_try_blockalign(bs, bytes) };
    if (!r.buf) {
        return -ENOMEM;
    }
    int ret = thread_pool_submit_co(smb_read_worker, &r);
    if (ret == 0) {
        qemu_iovec_from_buf(qiov, 0, r.buf, bytes);
    }
    qemu_vfree(r.buf);
    return ret;
}

static int coroutine_fn smb_co_pwritev(BlockDriverState *bs, int64_t offset, int64_t bytes, QEMUIOVector *qiov,
                                       BdrvRequestFlags flags)
{
    return -EROFS;
}

static int64_t coroutine_fn smb_co_getlength(BlockDriverState *bs)
{
    BDRVSmbState *s = bs->opaque;
    return s->length;
}

static BlockDriver bdrv_uwp_smb = {
    .format_name         = "smb",
    .protocol_name       = "smb",
    .instance_size       = sizeof(BDRVSmbState),
    .bdrv_parse_filename = smb_parse_filename,
    .bdrv_open           = smb_file_open,
    .bdrv_close          = smb_file_close,
    .bdrv_co_preadv      = smb_co_preadv,
    .bdrv_co_pwritev     = smb_co_pwritev,
    .bdrv_co_getlength   = smb_co_getlength,
};

static void bdrv_uwp_smb_init(void)
{
    bdrv_register(&bdrv_uwp_smb);
}

block_init(bdrv_uwp_smb_init);

/* A few bytes of a network disc image without opening it as a drive (ui/xemu.c: where the game partition of a
   full disc dump starts) */
bool xemu_uwp_smb_peek(const char *url, uint64_t offset, void *buf, uint32_t len)
{
    char *server = NULL, *share = NULL, *path = NULL;
    bool ok = false;
    if (!smb_split(url, &server, &share, &path)) {
        return false;
    }
    Error *err = NULL;
    struct smb2_context *c = smb_connect(server, share, &err);
    if (!c) {
        uwp_log("%s", error_get_pretty(err));
        error_free(err);
    } else {
        struct smb2fh *fh = smb2_open(c, path, O_RDONLY);
        if (fh) {
            uint32_t done = 0;
            while (done < len) {
                int got = smb2_pread(c, fh, (uint8_t *)buf + done, len - done, offset + done);
                if (got <= 0) {
                    break;
                }
                done += got;
            }
            ok = done == len;
            smb2_close(c, fh);
        }
        smb2_disconnect_share(c);
        smb2_destroy_context(c);
    }
    g_free(server);
    g_free(share);
    g_free(path);
    return ok;
}

#endif /* XEMU_UWP */
