/*
 * QEMU System Emulator
 *
 * Copyright (c) 2003-2008 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#ifdef XEMU_UWP
#include <SDL3/SDL.h>
#endif
#include "qemu/log.h"
#include "net/slirp.h"


#if defined(CONFIG_SMBD_COMMAND)
#include <pwd.h>
#include <sys/wait.h>
#endif
#include "net/eth.h"
#include "net/net.h"
#include "clients.h"
#include "hub.h"
#include "monitor/monitor.h"
#include "qemu/error-report.h"
#include "qemu/sockets.h"
#include <libslirp.h>
#include "chardev/char-fe.h"
#include "system/system.h"
#include "qemu/cutils.h"
#include "qapi/error.h"
#include "qobject/qdict.h"
#include "util.h"
#include "migration/register.h"
#include "migration/vmstate.h"
#include "migration/qemu-file-types.h"

static int get_str_sep(char *buf, int buf_size, const char **pp, int sep)
{
    const char *p, *p1;
    int len;
    p = *pp;
    p1 = strchr(p, sep);
    if (!p1)
        return -1;
    len = p1 - p;
    p1++;
    if (buf_size > 0) {
        if (len > buf_size - 1)
            len = buf_size - 1;
        memcpy(buf, p, len);
        buf[len] = '\0';
    }
    *pp = p1;
    return 0;
}

/* slirp network adapter */

#define SLIRP_CFG_HOSTFWD 1

struct slirp_config_str {
    struct slirp_config_str *next;
    int flags;
    char str[1024];
};

struct GuestFwd {
    CharFrontend hd;
    struct in_addr server;
    int port;
    Slirp *slirp;
};

typedef struct SlirpState {
    NetClientState nc;
    QTAILQ_ENTRY(SlirpState) entry;
    Slirp *slirp;
    Notifier poll_notifier;
    Notifier exit_notifier;
#if defined(CONFIG_SMBD_COMMAND)
    gchar *smb_dir;
#endif
    GSList *fwd;
} SlirpState;

static struct slirp_config_str *slirp_configs;
static QTAILQ_HEAD(, SlirpState) slirp_stacks =
    QTAILQ_HEAD_INITIALIZER(slirp_stacks);

static int slirp_hostfwd(SlirpState *s, const char *redir_str, Error **errp);
static int slirp_guestfwd(SlirpState *s, const char *config_str, Error **errp);

#if defined(CONFIG_SMBD_COMMAND)
static int slirp_smb(SlirpState *s, const char *exported_dir,
                     struct in_addr vserver_addr, Error **errp);
static void slirp_smb_cleanup(SlirpState *s);
#else
static inline void slirp_smb_cleanup(SlirpState *s) { }
#endif

#ifdef XEMU_UWP
/*
 * Game Vault: Xbox Live lookups go to the network the game plays on (ui/xemu.c, uwp_xnet_switch). A DNS question
 * from the Xbox for a name under xboxlive.com is sent to that network's resolver, whatever DNS server the Xbox has
 * set (Insignia's, or xemu's own), and the answer comes back looking as if that server sent it. Other names are
 * left alone: XB Online's resolver answers only its own service names.
 */
extern int xemu_uwp_xnet;
extern uint32_t xemu_uwp_xnet_dns(void);

typedef struct {
    uint16_t port, id;
    uint32_t orig;
    bool used;
} XnetAsk;
static XnetAsk xnet_asks[64];
static unsigned xnet_next;

static uint16_t xnet_sum16(const uint8_t *p, size_t len, uint32_t sum)
{
    for (size_t i = 0; i + 1 < len; i += 2) {
        sum += (uint32_t)p[i] << 8 | p[i + 1];
    }
    if (len & 1) {
        sum += (uint32_t)p[len - 1] << 8;
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (uint16_t)sum;
}

/* IPv4 header and UDP checksums again after an address change */
static void xnet_fix_sums(uint8_t *ip, size_t ihl, size_t total)
{
    ip[10] = ip[11] = 0;
    uint16_t s = ~xnet_sum16(ip, ihl, 0);
    ip[10] = s >> 8;
    ip[11] = s & 0xFF;
    uint8_t *udp = ip + ihl;
    size_t ulen = total - ihl;
    udp[6] = udp[7] = 0;
    uint8_t pseudo[12];
    memcpy(pseudo, ip + 12, 8);
    pseudo[8] = 0;
    pseudo[9] = 17;
    pseudo[10] = ulen >> 8;
    pseudo[11] = ulen & 0xFF;
    uint32_t sum = xnet_sum16(pseudo, sizeof(pseudo), 0);
    uint16_t u = ~xnet_sum16(udp, ulen, sum);
    if (!u) {
        u = 0xFFFF;
    }
    udp[6] = u >> 8;
    udp[7] = u & 0xFF;
}

/* the first question's name ends in xboxlive.com */
static bool xnet_live_name(const uint8_t *dns, size_t len)
{
    char name[256];
    size_t at = 12, n = 0;
    if (len < 17 || (dns[4] == 0 && dns[5] == 0)) {
        return false;
    }
    while (at < len && dns[at]) {
        size_t l = dns[at++];
        if (l > 63 || at + l > len || n + l + 1 >= sizeof(name)) {
            return false;
        }
        if (n) {
            name[n++] = '.';
        }
        for (size_t i = 0; i < l; i++) {
            name[n++] = g_ascii_tolower(dns[at + i]);
        }
        at += l;
    }
    name[n] = 0;
    return g_str_has_suffix(name, "xboxlive.com");
}

/* an Ethernet frame's IPv4/UDP parts; false when it is not IPv4 UDP */
static bool xnet_udp(const uint8_t *buf, size_t size, size_t *ihl, size_t *total)
{
    if (size < 14 + 20 + 8 || buf[12] != 0x08 || buf[13] != 0x00) {
        return false;
    }
    const uint8_t *ip = buf + 14;
    *ihl = (ip[0] & 0x0F) * 4;
    *total = (size_t)ip[2] << 8 | ip[3];
    return (ip[0] >> 4) == 4 && ip[9] == 17 && *ihl >= 20 && *total >= *ihl + 8 && 14 + *total <= size &&
           !(ip[6] & 0x3F) && !ip[7];
}

/* from the Xbox: a Live lookup goes to the chosen network's resolver; returns a changed copy or NULL */
static uint8_t *xnet_out(const uint8_t *buf, size_t size)
{
    size_t ihl, total;
    uint32_t to = xemu_uwp_xnet ? xemu_uwp_xnet_dns() : 0;
    if (!to || !xnet_udp(buf, size, &ihl, &total)) {
        return NULL;
    }
    const uint8_t *udp = buf + 14 + ihl;
    if ((udp[2] << 8 | udp[3]) != 53 || !xnet_live_name(udp + 8, total - ihl - 8)) {
        return NULL;
    }
    uint32_t orig;
    memcpy(&orig, buf + 14 + 16, 4);
    if (orig == to) {
        return NULL;
    }
    XnetAsk *a = &xnet_asks[xnet_next++ % G_N_ELEMENTS(xnet_asks)];
    a->port = udp[0] << 8 | udp[1];
    a->id = udp[8] << 8 | udp[9];
    a->orig = orig;
    a->used = true;
    uint8_t *copy = g_memdup2(buf, size);
    memcpy(copy + 14 + 16, &to, 4);
    xnet_fix_sums(copy + 14, ihl, total);
    return copy;
}

/* to the Xbox: the answer from that resolver looks as if it came from the server the Xbox asked */
static uint8_t *xnet_in(const uint8_t *buf, size_t size)
{
    size_t ihl, total;
    uint32_t from = xemu_uwp_xnet ? xemu_uwp_xnet_dns() : 0;
    if (!from || !xnet_udp(buf, size, &ihl, &total)) {
        return NULL;
    }
    const uint8_t *udp = buf + 14 + ihl;
    uint32_t src;
    memcpy(&src, buf + 14 + 12, 4);
    if ((udp[0] << 8 | udp[1]) != 53 || src != from || total < ihl + 8 + 12) {
        return NULL;
    }
    uint16_t port = udp[2] << 8 | udp[3], id = udp[8] << 8 | udp[9];
    for (size_t i = 0; i < G_N_ELEMENTS(xnet_asks); i++) {
        XnetAsk *a = &xnet_asks[i];
        if (a->used && a->port == port && a->id == id) {
            a->used = false;
            uint8_t *copy = g_memdup2(buf, size);
            memcpy(copy + 14 + 12, &a->orig, 4);
            xnet_fix_sums(copy + 14, ihl, total);
            return copy;
        }
    }
    return NULL;
}
#endif

#ifdef XEMU_UWP
/*
 * XB Online without a resolver: xemu answers the Xbox's Xbox Live lookups (*.xboxlive.com) itself with xb.live's
 * service address (Game Vault's link xnetip=, 0 = ask a resolver as above). Every Xbox Live service of XB Online is
 * on that one server, so its address is the whole answer. Returns the reply frame (to send to the Xbox) or NULL.
 */
extern uint32_t xemu_uwp_xnet_answer_ip;

static uint8_t *xnet_answer(const uint8_t *buf, size_t size, size_t *out_len)
{
    size_t ihl, total;
    if (xemu_uwp_xnet != 2 || !xemu_uwp_xnet_answer_ip || !xnet_udp(buf, size, &ihl, &total)) {
        return NULL;
    }
    const uint8_t *udp = buf + 14 + ihl, *dns = udp + 8;
    size_t dlen = total - ihl - 8;
    if ((udp[2] << 8 | udp[3]) != 53 || !xnet_live_name(dns, dlen) || (dns[2] & 0x80)) {
        return NULL;
    }
    size_t q = 12;   /* end of the first question: its name, then type and class */
    while (q < dlen && dns[q]) {
        q += dns[q] + 1;
    }
    q += 1 + 4;
    if (q > dlen) {
        return NULL;
    }
    bool type_a = dns[q - 4] == 0 && dns[q - 3] == 1;
    size_t ans = type_a ? 16 : 0, rdns = q + ans, rlen = 14 + 20 + 8 + rdns;
    uint8_t *r = g_malloc0(rlen);
    memcpy(r, buf + 6, 6);          /* to the Xbox's address */
    memcpy(r + 6, buf, 6);          /* from the address it sent to */
    r[12] = 0x08, r[13] = 0x00;
    uint8_t *ip = r + 14;
    ip[0] = 0x45;
    ip[2] = (20 + 8 + rdns) >> 8, ip[3] = (20 + 8 + rdns) & 0xFF;
    ip[8] = 64, ip[9] = 17;
    memcpy(ip + 12, buf + 14 + 16, 4);   /* from the DNS server the Xbox asked */
    memcpy(ip + 16, buf + 14 + 12, 4);   /* to the Xbox */
    uint8_t *ru = ip + 20;
    ru[0] = 0, ru[1] = 53;
    ru[2] = udp[0], ru[3] = udp[1];
    ru[4] = (8 + rdns) >> 8, ru[5] = (8 + rdns) & 0xFF;
    uint8_t *rd = ru + 8;
    memcpy(rd, dns, q);
    rd[2] = 0x84 | (dns[2] & 0x01);      /* an answer, authoritative, recursion as asked */
    rd[3] = 0x80;                        /* recursion available, no error */
    rd[4] = 0, rd[5] = 1;                /* one question */
    rd[6] = 0, rd[7] = type_a ? 1 : 0;   /* one answer for an address question */
    rd[8] = rd[9] = rd[10] = rd[11] = 0;
    if (type_a) {
        uint8_t *a = rd + q;
        a[0] = 0xC0, a[1] = 0x0C;        /* the question's name */
        a[2] = 0, a[3] = 1, a[4] = 0, a[5] = 1;
        a[6] = a[7] = a[8] = 0, a[9] = 60;   /* 60 s */
        a[10] = 0, a[11] = 4;
        memcpy(a + 12, &xemu_uwp_xnet_answer_ip, 4);
    }
    xnet_fix_sums(ip, 20, 20 + 8 + rdns);
    *out_len = rlen;
    return r;
}
#endif

#ifdef XEMU_UWP
/* test trace (xnet-trace.txt in xemu's folder): Live lookups and the first packets to and from XB Online's server */
extern void uwp_log(const char *fmt, ...);
static int xnet_trace = -1;
static void xnet_trace_pkt(const char *dir, const uint8_t *buf, size_t size)
{
    if (xnet_trace < 0) {
        char *own = SDL_GetPrefPath("xemu", "xemu");
        char *f = g_strdup_printf("%sxnet-trace.txt", own ? own : "");
        xnet_trace = g_file_test(f, G_FILE_TEST_EXISTS) ? 200 : 0;
        g_free(f);
        SDL_free(own);
    }
    if (xnet_trace <= 0 || size < 34 || buf[12] != 0x08 || buf[13] != 0) {
        return;
    }
    const uint8_t *ip = buf + 14;
    size_t ihl = (ip[0] & 15) * 4;
    uint32_t a = xemu_uwp_xnet_answer_ip ? xemu_uwp_xnet_answer_ip : xemu_uwp_xnet_dns();
    uint32_t src, dst;
    memcpy(&src, ip + 12, 4);
    memcpy(&dst, ip + 16, 4);
    bool dns = (ip[9] == 17) && size >= 14 + ihl + 4 && ((buf[14 + ihl + 2] << 8 | buf[14 + ihl + 3]) == 53 || (buf[14 + ihl] << 8 | buf[14 + ihl + 1]) == 53);
    if (!a || (src != a && dst != a && !dns)) {
        return;
    }
    xnet_trace--;
    uwp_log("xnet %s proto %u %u.%u.%u.%u:%u -> %u.%u.%u.%u:%u len %u", dir, ip[9], ip[12], ip[13], ip[14], ip[15],
            size >= 14 + ihl + 4 ? (buf[14 + ihl] << 8 | buf[14 + ihl + 1]) : 0, ip[16], ip[17], ip[18], ip[19],
            size >= 14 + ihl + 4 ? (buf[14 + ihl + 2] << 8 | buf[14 + ihl + 3]) : 0, (unsigned)size);
}
#endif

static ssize_t net_slirp_send_packet(const void *pkt, size_t pkt_len,
                                     void *opaque)
{
    SlirpState *s = opaque;
    uint8_t min_pkt[ETH_ZLEN];
    size_t min_pktsz = sizeof(min_pkt);
#ifdef XEMU_UWP
    xnet_trace_pkt("in ", pkt, pkt_len);
    g_autofree uint8_t *xnet = xnet_in(pkt, pkt_len);
    if (xnet) {
        pkt = xnet;
    }
#endif

    if (net_peer_needs_padding(&s->nc)) {
        if (eth_pad_short_frame(min_pkt, &min_pktsz, pkt, pkt_len)) {
            pkt = min_pkt;
            pkt_len = min_pktsz;
        }
    }

    return qemu_send_packet(&s->nc, pkt, pkt_len);
}

static ssize_t net_slirp_receive(NetClientState *nc, const uint8_t *buf, size_t size)
{
    SlirpState *s = DO_UPCAST(SlirpState, nc, nc);

#ifdef XEMU_UWP
    xnet_trace_pkt("out", buf, size);
    size_t answer_len = 0;
    g_autofree uint8_t *answer = xnet_answer(buf, size, &answer_len);
    if (answer) {
        qemu_send_packet(&s->nc, answer, answer_len);   /* the reply goes straight back to the Xbox */
        return size;
    }
    g_autofree uint8_t *xnet = xnet_out(buf, size);
    if (xnet) {
        slirp_input(s->slirp, xnet, size);
        return size;
    }
#endif
    slirp_input(s->slirp, buf, size);

    return size;
}

static void slirp_smb_exit(Notifier *n, void *data)
{
    SlirpState *s = container_of(n, SlirpState, exit_notifier);
    slirp_smb_cleanup(s);
}

static void slirp_free_fwd(gpointer data)
{
    struct GuestFwd *fwd = data;

    qemu_chr_fe_deinit(&fwd->hd, true);
    g_free(data);
}

static void net_slirp_cleanup(NetClientState *nc)
{
    SlirpState *s = DO_UPCAST(SlirpState, nc, nc);

    g_slist_free_full(s->fwd, slirp_free_fwd);
    main_loop_poll_remove_notifier(&s->poll_notifier);
    unregister_savevm(NULL, "slirp", s->slirp);
    slirp_cleanup(s->slirp);
    if (s->exit_notifier.notify) {
        qemu_remove_exit_notifier(&s->exit_notifier);
    }
    slirp_smb_cleanup(s);
    QTAILQ_REMOVE(&slirp_stacks, s, entry);
}

static NetClientInfo net_slirp_info = {
    .type = NET_CLIENT_DRIVER_USER,
    .size = sizeof(SlirpState),
    .receive = net_slirp_receive,
    .cleanup = net_slirp_cleanup,
};

static void net_slirp_guest_error(const char *msg, void *opaque)
{
    qemu_log_mask(LOG_GUEST_ERROR, "%s", msg);
}

static int64_t net_slirp_clock_get_ns(void *opaque)
{
    return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

typedef struct SlirpTimer SlirpTimer;
struct SlirpTimer {
    QEMUTimer timer;
#if SLIRP_CHECK_VERSION(4,7,0)
    Slirp *slirp;
    SlirpTimerId id;
    void *cb_opaque;
#endif
};

#if SLIRP_CHECK_VERSION(4,7,0)
static void net_slirp_init_completed(Slirp *slirp, void *opaque)
{
    SlirpState *s = opaque;
    s->slirp = slirp;
}

static void net_slirp_timer_cb(void *opaque)
{
    SlirpTimer *t = opaque;
    slirp_handle_timer(t->slirp, t->id, t->cb_opaque);
}

static void *net_slirp_timer_new_opaque(SlirpTimerId id,
                                        void *cb_opaque, void *opaque)
{
    SlirpState *s = opaque;
    SlirpTimer *t = g_new(SlirpTimer, 1);
    t->slirp = s->slirp;
    t->id = id;
    t->cb_opaque = cb_opaque;
    timer_init_full(&t->timer, NULL, QEMU_CLOCK_VIRTUAL,
                    SCALE_MS, QEMU_TIMER_ATTR_EXTERNAL,
                    net_slirp_timer_cb, t);
    return t;
}
#else
static void *net_slirp_timer_new(SlirpTimerCb cb,
                                 void *cb_opaque, void *opaque)
{
    SlirpTimer *t = g_new(SlirpTimer, 1);
    timer_init_full(&t->timer, NULL, QEMU_CLOCK_VIRTUAL,
                    SCALE_MS, QEMU_TIMER_ATTR_EXTERNAL,
                    cb, cb_opaque);
    return t;
}
#endif

static void net_slirp_timer_free(void *timer, void *opaque)
{
    SlirpTimer *t = timer;
    timer_del(&t->timer);
    g_free(t);
}

static void net_slirp_timer_mod(void *timer, int64_t expire_timer,
                                void *opaque)
{
    SlirpTimer *t = timer;
    timer_mod(&t->timer, expire_timer);
}

#if !SLIRP_CHECK_VERSION(4, 9, 0)
# define slirp_os_socket int
# define slirp_pollfds_fill_socket slirp_pollfds_fill
# define register_poll_socket register_poll_fd
# define unregister_poll_socket unregister_poll_fd
#endif

static void net_slirp_register_poll_sock(slirp_os_socket fd, void *opaque)
{
#ifdef WIN32
    AioContext *ctxt = qemu_get_aio_context();
    g_autofree char *msg = NULL;

    if (WSAEventSelect(fd, event_notifier_get_handle(&ctxt->notifier),
                       FD_READ | FD_ACCEPT | FD_CLOSE |
                       FD_CONNECT | FD_WRITE | FD_OOB) != 0) {
        msg = g_win32_error_message(WSAGetLastError());
        warn_report("failed to WSAEventSelect(): %s", msg);
    }
#endif
}

static void net_slirp_unregister_poll_sock(slirp_os_socket fd, void *opaque)
{
#ifdef WIN32
    g_autofree char *msg = NULL;

    if (WSAEventSelect(fd, NULL, 0) != 0) {
        msg = g_win32_error_message(WSAGetLastError());
        warn_report("failed to WSAEventSelect(): %s", msg);
    }
#endif
}

static void net_slirp_notify(void *opaque)
{
    qemu_notify_event();
}

static const SlirpCb slirp_cb = {
    .send_packet = net_slirp_send_packet,
    .guest_error = net_slirp_guest_error,
    .clock_get_ns = net_slirp_clock_get_ns,
#if SLIRP_CHECK_VERSION(4,7,0)
    .init_completed = net_slirp_init_completed,
    .timer_new_opaque = net_slirp_timer_new_opaque,
#else
    .timer_new = net_slirp_timer_new,
#endif
    .timer_free = net_slirp_timer_free,
    .timer_mod = net_slirp_timer_mod,
    .register_poll_socket = net_slirp_register_poll_sock,
    .unregister_poll_socket = net_slirp_unregister_poll_sock,
    .notify = net_slirp_notify,
};

static int slirp_poll_to_gio(int events)
{
    int ret = 0;

    if (events & SLIRP_POLL_IN) {
        ret |= G_IO_IN;
    }
    if (events & SLIRP_POLL_OUT) {
        ret |= G_IO_OUT;
    }
    if (events & SLIRP_POLL_PRI) {
        ret |= G_IO_PRI;
    }
    if (events & SLIRP_POLL_ERR) {
        ret |= G_IO_ERR;
    }
    if (events & SLIRP_POLL_HUP) {
        ret |= G_IO_HUP;
    }

    return ret;
}

static int net_slirp_add_poll(slirp_os_socket fd, int events, void *opaque)
{
    GArray *pollfds = opaque;
    GPollFD pfd = {
        .fd = fd,
        .events = slirp_poll_to_gio(events),
    };
    int idx = pollfds->len;
    g_array_append_val(pollfds, pfd);
    return idx;
}

static int slirp_gio_to_poll(int events)
{
    int ret = 0;

    if (events & G_IO_IN) {
        ret |= SLIRP_POLL_IN;
    }
    if (events & G_IO_OUT) {
        ret |= SLIRP_POLL_OUT;
    }
    if (events & G_IO_PRI) {
        ret |= SLIRP_POLL_PRI;
    }
    if (events & G_IO_ERR) {
        ret |= SLIRP_POLL_ERR;
    }
    if (events & G_IO_HUP) {
        ret |= SLIRP_POLL_HUP;
    }

    return ret;
}

static int net_slirp_get_revents(int idx, void *opaque)
{
    GArray *pollfds = opaque;

    return slirp_gio_to_poll(g_array_index(pollfds, GPollFD, idx).revents);
}

static void net_slirp_poll_notify(Notifier *notifier, void *data)
{
    MainLoopPoll *poll = data;
    SlirpState *s = container_of(notifier, SlirpState, poll_notifier);

    switch (poll->state) {
    case MAIN_LOOP_POLL_FILL:
        slirp_pollfds_fill_socket(s->slirp, &poll->timeout,
                                  net_slirp_add_poll, poll->pollfds);
        break;
    case MAIN_LOOP_POLL_OK:
    case MAIN_LOOP_POLL_ERR:
        slirp_pollfds_poll(s->slirp, poll->state == MAIN_LOOP_POLL_ERR,
                           net_slirp_get_revents, poll->pollfds);
        break;
    default:
        g_assert_not_reached();
    }
}

static ssize_t
net_slirp_stream_read(void *buf, size_t size, void *opaque)
{
    QEMUFile *f = opaque;

    return qemu_get_buffer(f, buf, size);
}

static ssize_t
net_slirp_stream_write(const void *buf, size_t size, void *opaque)
{
    QEMUFile *f = opaque;

    qemu_put_buffer(f, buf, size);
    if (qemu_file_get_error(f)) {
        return -1;
    }

    return size;
}

static int net_slirp_state_load(QEMUFile *f, void *opaque, int version_id)
{
    Slirp *slirp = opaque;

    return slirp_state_load(slirp, version_id, net_slirp_stream_read, f);
}

static void net_slirp_state_save(QEMUFile *f, void *opaque)
{
    Slirp *slirp = opaque;

    slirp_state_save(slirp, net_slirp_stream_write, f);
}

static SaveVMHandlers savevm_slirp_state = {
    .save_state = net_slirp_state_save,
    .load_state = net_slirp_state_load,
};

static int net_slirp_init(NetClientState *peer, const char *model,
                          const char *name, int restricted,
                          bool ipv4, const char *vnetwork, const char *vhost,
                          bool ipv6, const char *vprefix6, int vprefix6_len,
                          const char *vhost6,
                          const char *vhostname, const char *tftp_export,
                          const char *bootfile, const char *vdhcp_start,
                          const char *vnameserver, const char *vnameserver6,
                          const char *smb_export, const char *vsmbserver,
                          const char **dnssearch, const char *vdomainname,
                          const char *tftp_server_name,
                          Error **errp)
{
    /* default settings according to historic slirp */
    struct in_addr net  = { .s_addr = htonl(0x0a000200) }; /* 10.0.2.0 */
    struct in_addr mask = { .s_addr = htonl(0xffffff00) }; /* 255.255.255.0 */
    struct in_addr host = { .s_addr = htonl(0x0a000202) }; /* 10.0.2.2 */
    struct in_addr dhcp = { .s_addr = htonl(0x0a00020f) }; /* 10.0.2.15 */
    struct in_addr dns  = { .s_addr = htonl(0x0a000203) }; /* 10.0.2.3 */
    struct in6_addr ip6_prefix;
    struct in6_addr ip6_host;
    struct in6_addr ip6_dns;
#if defined(CONFIG_SMBD_COMMAND)
    struct in_addr smbsrv = { .s_addr = 0 };
#endif
    SlirpConfig cfg = { 0 };
    NetClientState *nc;
    SlirpState *s;
    char buf[20];
    uint32_t addr;
    int shift;
    char *end;
    struct slirp_config_str *config;

    if (!ipv4 && (vnetwork || vhost || vnameserver)) {
        error_setg(errp, "IPv4 disabled but netmask/host/dns provided");
        return -1;
    }

    if (!ipv6 && (vprefix6 || vhost6 || vnameserver6)) {
        error_setg(errp, "IPv6 disabled but prefix/host6/dns6 provided");
        return -1;
    }

    if (!ipv4 && !ipv6) {
        /* It doesn't make sense to disable both */
        error_setg(errp, "IPv4 and IPv6 disabled");
        return -1;
    }

    if (vnetwork) {
        if (get_str_sep(buf, sizeof(buf), &vnetwork, '/') < 0) {
            if (!inet_aton(vnetwork, &net)) {
                error_setg(errp, "Failed to parse netmask");
                return -1;
            }
            addr = ntohl(net.s_addr);
            if (!(addr & 0x80000000)) {
                mask.s_addr = htonl(0xff000000); /* class A */
            } else if ((addr & 0xfff00000) == 0xac100000) {
                mask.s_addr = htonl(0xfff00000); /* priv. 172.16.0.0/12 */
            } else if ((addr & 0xc0000000) == 0x80000000) {
                mask.s_addr = htonl(0xffff0000); /* class B */
            } else if ((addr & 0xffff0000) == 0xc0a80000) {
                mask.s_addr = htonl(0xffff0000); /* priv. 192.168.0.0/16 */
            } else if ((addr & 0xffff0000) == 0xc6120000) {
                mask.s_addr = htonl(0xfffe0000); /* tests 198.18.0.0/15 */
            } else if ((addr & 0xe0000000) == 0xe0000000) {
                mask.s_addr = htonl(0xffffff00); /* class C */
            } else {
                mask.s_addr = htonl(0xfffffff0); /* multicast/reserved */
            }
        } else {
            if (!inet_aton(buf, &net)) {
                error_setg(errp, "Failed to parse netmask");
                return -1;
            }
            shift = strtol(vnetwork, &end, 10);
            if (*end != '\0') {
                if (!inet_aton(vnetwork, &mask)) {
                    error_setg(errp,
                               "Failed to parse netmask (trailing chars)");
                    return -1;
                }
            } else if (shift < 4 || shift > 32) {
                error_setg(errp,
                           "Invalid netmask provided (must be in range 4-32)");
                return -1;
            } else {
                mask.s_addr = htonl(0xffffffff << (32 - shift));
            }
        }
        net.s_addr &= mask.s_addr;
        host.s_addr = net.s_addr | (htonl(0x0202) & ~mask.s_addr);
        dhcp.s_addr = net.s_addr | (htonl(0x020f) & ~mask.s_addr);
        dns.s_addr  = net.s_addr | (htonl(0x0203) & ~mask.s_addr);
    }

    if (vhost && !inet_aton(vhost, &host)) {
        error_setg(errp, "Failed to parse host");
        return -1;
    }
    if ((host.s_addr & mask.s_addr) != net.s_addr) {
        error_setg(errp, "Host doesn't belong to network");
        return -1;
    }

    if (vnameserver && !inet_aton(vnameserver, &dns)) {
        error_setg(errp, "Failed to parse DNS");
        return -1;
    }
    if (restricted && (dns.s_addr & mask.s_addr) != net.s_addr) {
        error_setg(errp, "DNS doesn't belong to network");
        return -1;
    }
    if (dns.s_addr == host.s_addr) {
        error_setg(errp, "DNS must be different from host");
        return -1;
    }

    if (vdhcp_start && !inet_aton(vdhcp_start, &dhcp)) {
        error_setg(errp, "Failed to parse DHCP start address");
        return -1;
    }
    if ((dhcp.s_addr & mask.s_addr) != net.s_addr) {
        error_setg(errp, "DHCP doesn't belong to network");
        return -1;
    }
    if (dhcp.s_addr == host.s_addr || dhcp.s_addr == dns.s_addr) {
        error_setg(errp, "DHCP must be different from host and DNS");
        return -1;
    }

#if defined(CONFIG_SMBD_COMMAND)
    if (vsmbserver && !inet_aton(vsmbserver, &smbsrv)) {
        error_setg(errp, "Failed to parse SMB address");
        return -1;
    }
#endif

    if (!vprefix6) {
        vprefix6 = "fec0::";
    }
    if (!inet_pton(AF_INET6, vprefix6, &ip6_prefix)) {
        error_setg(errp, "Failed to parse IPv6 prefix");
        return -1;
    }

    if (!vprefix6_len) {
        vprefix6_len = 64;
    }
    if (vprefix6_len < 0 || vprefix6_len > 126) {
        error_setg(errp,
                   "Invalid IPv6 prefix provided "
                   "(IPv6 prefix length must be between 0 and 126)");
        return -1;
    }

    if (vhost6) {
        if (!inet_pton(AF_INET6, vhost6, &ip6_host)) {
            error_setg(errp, "Failed to parse IPv6 host");
            return -1;
        }
        if (!in6_equal_net(&ip6_prefix, &ip6_host, vprefix6_len)) {
            error_setg(errp, "IPv6 Host doesn't belong to network");
            return -1;
        }
    } else {
        ip6_host = ip6_prefix;
        ip6_host.s6_addr[15] |= 2;
    }

    if (vnameserver6) {
        if (!inet_pton(AF_INET6, vnameserver6, &ip6_dns)) {
            error_setg(errp, "Failed to parse IPv6 DNS");
            return -1;
        }
        if (restricted && !in6_equal_net(&ip6_prefix, &ip6_dns, vprefix6_len)) {
            error_setg(errp, "IPv6 DNS doesn't belong to network");
            return -1;
        }
    } else {
        ip6_dns = ip6_prefix;
        ip6_dns.s6_addr[15] |= 3;
    }

    if (vdomainname && !*vdomainname) {
        error_setg(errp, "'domainname' parameter cannot be empty");
        return -1;
    }

    if (vdomainname && strlen(vdomainname) > 255) {
        error_setg(errp, "'domainname' parameter cannot exceed 255 bytes");
        return -1;
    }

    if (vhostname && strlen(vhostname) > 255) {
        error_setg(errp, "'vhostname' parameter cannot exceed 255 bytes");
        return -1;
    }

    if (tftp_server_name && strlen(tftp_server_name) > 255) {
        error_setg(errp, "'tftp-server-name' parameter cannot exceed 255 bytes");
        return -1;
    }

    nc = qemu_new_net_client(&net_slirp_info, peer, model, name);

    qemu_set_info_str(nc, "net=%s,restrict=%s", inet_ntoa(net),
                      restricted ? "on" : "off");

    s = DO_UPCAST(SlirpState, nc, nc);

    cfg.version =
         SLIRP_CHECK_VERSION(4, 9, 0) ? 6 :
         SLIRP_CHECK_VERSION(4, 7, 0) ? 4 : 1;
    cfg.restricted = restricted;
    cfg.in_enabled = ipv4;
    cfg.vnetwork = net;
    cfg.vnetmask = mask;
    cfg.vhost = host;
    cfg.in6_enabled = ipv6;
    cfg.vprefix_addr6 = ip6_prefix;
    cfg.vprefix_len = vprefix6_len;
    cfg.vhost6 = ip6_host;
    cfg.vhostname = vhostname;
    cfg.tftp_server_name = tftp_server_name;
    cfg.tftp_path = tftp_export;
    cfg.bootfile = bootfile;
    cfg.vdhcp_start = dhcp;
    cfg.vnameserver = dns;
    cfg.vnameserver6 = ip6_dns;
    cfg.vdnssearch = dnssearch;
    cfg.vdomainname = vdomainname;
    s->slirp = slirp_new(&cfg, &slirp_cb, s);
    QTAILQ_INSERT_TAIL(&slirp_stacks, s, entry);

    /*
     * Make sure the current bitstream version of slirp is 4, to avoid
     * QEMU migration incompatibilities, if upstream slirp bumped the
     * version.
     *
     * FIXME: use bitfields of features? teach libslirp to save with
     * specific version?
     */
    g_assert(slirp_state_version() == 4);
    register_savevm_live("slirp", VMSTATE_INSTANCE_ID_ANY,
                         slirp_state_version(), &savevm_slirp_state, s->slirp);

    s->poll_notifier.notify = net_slirp_poll_notify;
    main_loop_poll_add_notifier(&s->poll_notifier);

    for (config = slirp_configs; config; config = config->next) {
        if (config->flags & SLIRP_CFG_HOSTFWD) {
            if (slirp_hostfwd(s, config->str, errp) < 0) {
                goto error;
            }
        } else {
            if (slirp_guestfwd(s, config->str, errp) < 0) {
                goto error;
            }
        }
    }
#if defined(CONFIG_SMBD_COMMAND)
    if (smb_export) {
        if (slirp_smb(s, smb_export, smbsrv, errp) < 0) {
            goto error;
        }
    }
#endif

    s->exit_notifier.notify = slirp_smb_exit;
    qemu_add_exit_notifier(&s->exit_notifier);
    return 0;

error:
    qemu_del_net_client(nc);
    return -1;
}

static SlirpState *slirp_lookup(Monitor *mon, const char *id)
{
    if (id) {
        NetClientState *nc = qemu_find_netdev(id);
        if (!nc) {
            monitor_printf(mon, "unrecognized netdev id '%s'\n", id);
            return NULL;
        }
        if (strcmp(nc->model, "user")) {
            monitor_printf(mon, "invalid device specified\n");
            return NULL;
        }
        return DO_UPCAST(SlirpState, nc, nc);
    } else {
        if (QTAILQ_EMPTY(&slirp_stacks)) {
            monitor_printf(mon, "user mode network stack not in use\n");
            return NULL;
        }
        return QTAILQ_FIRST(&slirp_stacks);
    }
}

#ifdef XBOX
void *slirp_get_state_from_netdev(const char *id)
{
    SlirpState *s = slirp_lookup(NULL, id);
    if (!s) return NULL;
    return s->slirp;
}
#endif

void hmp_hostfwd_remove(Monitor *mon, const QDict *qdict)
{
    /* TODO: support removing unix fwd */
    struct sockaddr_in host_addr = {
        .sin_family = AF_INET,
        .sin_addr = {
            .s_addr = INADDR_ANY,
        },
    };
    int host_port;
    char buf[256];
    const char *src_str, *p;
    SlirpState *s;
    int is_udp = 0;
    int err;
    const char *arg1 = qdict_get_str(qdict, "arg1");
    const char *arg2 = qdict_get_try_str(qdict, "arg2");

    if (arg2) {
        s = slirp_lookup(mon, arg1);
        src_str = arg2;
    } else {
        s = slirp_lookup(mon, NULL);
        src_str = arg1;
    }
    if (!s) {
        return;
    }

    p = src_str;
    if (!p || get_str_sep(buf, sizeof(buf), &p, ':') < 0) {
        goto fail_syntax;
    }

    if (!strcmp(buf, "tcp") || buf[0] == '\0') {
        is_udp = 0;
    } else if (!strcmp(buf, "udp")) {
        is_udp = 1;
    } else {
        goto fail_syntax;
    }

    if (get_str_sep(buf, sizeof(buf), &p, ':') < 0) {
        goto fail_syntax;
    }
    if (buf[0] != '\0' && !inet_aton(buf, &host_addr.sin_addr)) {
        goto fail_syntax;
    }

    if (qemu_strtoi(p, NULL, 10, &host_port)) {
        goto fail_syntax;
    }
    host_addr.sin_port = htons(host_port);

#if SLIRP_CHECK_VERSION(4, 5, 0)
    err = slirp_remove_hostxfwd(s->slirp, (struct sockaddr *) &host_addr,
            sizeof(host_addr), is_udp ? SLIRP_HOSTFWD_UDP : 0);
#else
    err = slirp_remove_hostfwd(s->slirp, is_udp, host_addr.sin_addr, host_port);
#endif

    monitor_printf(mon, "host forwarding rule for %s %s\n", src_str,
                   err ? "not found" : "removed");
    return;

 fail_syntax:
    monitor_printf(mon, "invalid format\n");
}

static int slirp_hostfwd(SlirpState *s, const char *redir_str, Error **errp)
{
    union {
        struct sockaddr_in in;
#if !defined(WIN32) && SLIRP_CHECK_VERSION(4, 7, 0)
        struct sockaddr_un un;
#endif
    } host_addr = {0};

    struct sockaddr_in guest_addr = {
        .sin_family = AF_INET,
        .sin_addr = {
            .s_addr = 0,
        },
    };
    int err;
    int host_port, guest_port;
    const char *p;
    char buf[256];
    int is_udp = 0;
#if !defined(WIN32) && SLIRP_CHECK_VERSION(4, 7, 0)
    int is_unix = 0;
#endif
    const char *end;
    const char *fail_reason = "Unknown reason";
    socklen_t host_addr_size;

    p = redir_str;
    if (!p || get_str_sep(buf, sizeof(buf), &p, ':') < 0) {
        fail_reason = "No : separators";
        goto fail_syntax;
    }
    if (!strcmp(buf, "tcp") || buf[0] == '\0') {
        is_udp = 0;
    } else if (!strcmp(buf, "udp")) {
        is_udp = 1;
    }
#if !defined(WIN32) && SLIRP_CHECK_VERSION(4, 7, 0)
    else if (!strcmp(buf, "unix")) {
        is_unix = 1;
    }
#endif
    else {
        fail_reason = "Bad protocol name";
        goto fail_syntax;
    }

#if !defined(WIN32) && SLIRP_CHECK_VERSION(4, 7, 0)
    if (is_unix) {
        if (get_str_sep(buf, sizeof(buf), &p, '-') < 0) {
            fail_reason = "Missing - separator";
            goto fail_syntax;
        }
        if (buf[0] == '\0') {
            fail_reason = "Missing unix socket path";
            goto fail_syntax;
        }
        if (buf[0] != '/') {
            fail_reason = "unix socket path must be absolute";
            goto fail_syntax;
        }

        size_t path_len = strlen(buf);
        if (path_len > sizeof(host_addr.un.sun_path) - 1) {
            fail_reason = "Unix socket path is too long";
            goto fail_syntax;
        }

        struct stat st;
        if (stat(buf, &st) == 0) {
            if (!S_ISSOCK(st.st_mode)) {
                fail_reason = "file exists and it's not unix socket";
                goto fail_syntax;
            }

            if (unlink(buf) < 0) {
                error_setg_errno(errp, errno, "Failed to unlink '%s'", buf);
                goto fail_syntax;
            }
        }
        host_addr.un.sun_family = AF_UNIX;
        memcpy(host_addr.un.sun_path, buf, path_len);
        host_addr_size = sizeof(host_addr.un);
    } else
#endif
    {
        host_addr.in.sin_family = AF_INET;
        host_addr.in.sin_addr.s_addr = INADDR_ANY;

        if (get_str_sep(buf, sizeof(buf), &p, ':') < 0) {
            fail_reason = "Missing : separator";
            goto fail_syntax;
        }

        if (buf[0] != '\0' && !inet_aton(buf, &host_addr.in.sin_addr)) {
            fail_reason = "Bad host address";
            goto fail_syntax;
        }

        if (get_str_sep(buf, sizeof(buf), &p, '-') < 0) {
            fail_reason = "Bad host port separator";
            goto fail_syntax;
        }

        err = qemu_strtoi(buf, &end, 0, &host_port);
        if (err || host_port < 0 || host_port > 65535) {
            fail_reason = "Bad host port";
            goto fail_syntax;
        }

        host_addr.in.sin_port = htons(host_port);
        host_addr_size = sizeof(host_addr.in);
    }

    if (get_str_sep(buf, sizeof(buf), &p, ':') < 0) {
        fail_reason = "Missing guest address";
        goto fail_syntax;
    }
    if (buf[0] != '\0' && !inet_aton(buf, &guest_addr.sin_addr)) {
        fail_reason = "Bad guest address";
        goto fail_syntax;
    }

    err = qemu_strtoi(p, &end, 0, &guest_port);
    if (err || guest_port < 1 || guest_port > 65535) {
        fail_reason = "Bad guest port";
        goto fail_syntax;
    }
    guest_addr.sin_port = htons(guest_port);

#if SLIRP_CHECK_VERSION(4, 5, 0)
    err = slirp_add_hostxfwd(s->slirp,
            (struct sockaddr *) &host_addr, host_addr_size,
            (struct sockaddr *) &guest_addr, sizeof(guest_addr),
            is_udp ? SLIRP_HOSTFWD_UDP : 0);
#else
    (void) host_addr_size;
    err = slirp_add_hostfwd(s->slirp, is_udp,
            host_addr.in.sin_addr, host_port,
            guest_addr.sin_addr, guest_port);
#endif

    if (err < 0) {
        error_setg(errp, "Could not set up host forwarding rule '%s'",
                   redir_str);
        return -1;
    }
    return 0;

 fail_syntax:
    error_setg(errp, "Invalid host forwarding rule '%s' (%s)", redir_str,
               fail_reason);
    return -1;
}

void hmp_hostfwd_add(Monitor *mon, const QDict *qdict)
{
    const char *redir_str;
    SlirpState *s;
    const char *arg1 = qdict_get_str(qdict, "arg1");
    const char *arg2 = qdict_get_try_str(qdict, "arg2");

    if (arg2) {
        s = slirp_lookup(mon, arg1);
        redir_str = arg2;
    } else {
        s = slirp_lookup(mon, NULL);
        redir_str = arg1;
    }
    if (s) {
        Error *err = NULL;
        if (slirp_hostfwd(s, redir_str, &err) < 0) {
            error_report_err(err);
        }
    }

}

#if defined(CONFIG_SMBD_COMMAND)

/* automatic user mode samba server configuration */
static void slirp_smb_cleanup(SlirpState *s)
{
    int ret;

    if (s->smb_dir) {
        gchar *cmd = g_strdup_printf("rm -rf %s", s->smb_dir);
        ret = system(cmd);
        if (ret == -1 || !WIFEXITED(ret)) {
            error_report("'%s' failed.", cmd);
        } else if (WEXITSTATUS(ret)) {
            error_report("'%s' failed. Error code: %d",
                         cmd, WEXITSTATUS(ret));
        }
        g_free(cmd);
        g_free(s->smb_dir);
        s->smb_dir = NULL;
    }
}

static int slirp_smb(SlirpState* s, const char *exported_dir,
                     struct in_addr vserver_addr, Error **errp)
{
    char *smb_conf;
    char *smb_cmdline;
    struct passwd *passwd;
    FILE *f;

    passwd = getpwuid(geteuid());
    if (!passwd) {
        error_setg(errp, "Failed to retrieve user name");
        return -1;
    }

    if (access(CONFIG_SMBD_COMMAND, F_OK)) {
        error_setg(errp, "Could not find '%s', please install it",
                   CONFIG_SMBD_COMMAND);
        return -1;
    }

    if (access(exported_dir, R_OK | X_OK)) {
        error_setg(errp, "Error accessing shared directory '%s': %s",
                   exported_dir, strerror(errno));
        return -1;
    }

    s->smb_dir = g_dir_make_tmp("qemu-smb.XXXXXX", NULL);
    if (!s->smb_dir) {
        error_setg(errp, "Could not create samba server dir");
        return -1;
    }
    smb_conf = g_strdup_printf("%s/%s", s->smb_dir, "smb.conf");

    f = fopen(smb_conf, "w");
    if (!f) {
        slirp_smb_cleanup(s);
        error_setg(errp,
                   "Could not create samba server configuration file '%s'",
                    smb_conf);
        g_free(smb_conf);
        return -1;
    }
    fprintf(f,
            "[global]\n"
            "private dir=%s\n"
            "interfaces=127.0.0.1\n"
            "bind interfaces only=yes\n"
            "pid directory=%s\n"
            "lock directory=%s\n"
            "state directory=%s\n"
            "cache directory=%s\n"
            "ncalrpc dir=%s/ncalrpc\n"
            "log file=%s/log.smbd\n"
            "smb passwd file=%s/smbpasswd\n"
            "security = user\n"
            "map to guest = Bad User\n"
            "load printers = no\n"
            "printing = bsd\n"
            "disable spoolss = yes\n"
            "usershare max shares = 0\n"
            "[qemu]\n"
            "path=%s\n"
            "read only=no\n"
            "guest ok=yes\n"
            "force user=%s\n",
            s->smb_dir,
            s->smb_dir,
            s->smb_dir,
            s->smb_dir,
            s->smb_dir,
            s->smb_dir,
            s->smb_dir,
            s->smb_dir,
            exported_dir,
            passwd->pw_name
            );
    fclose(f);

    smb_cmdline = g_strdup_printf("%s -l %s -s %s",
             CONFIG_SMBD_COMMAND, s->smb_dir, smb_conf);
    g_free(smb_conf);

    if (slirp_add_exec(s->slirp, smb_cmdline, &vserver_addr, 139) < 0 ||
        slirp_add_exec(s->slirp, smb_cmdline, &vserver_addr, 445) < 0) {
        slirp_smb_cleanup(s);
        g_free(smb_cmdline);
        error_setg(errp, "Conflicting/invalid smbserver address");
        return -1;
    }
    g_free(smb_cmdline);
    return 0;
}

#endif /* defined(CONFIG_SMBD_COMMAND) */

static int guestfwd_can_read(void *opaque)
{
    struct GuestFwd *fwd = opaque;
    return slirp_socket_can_recv(fwd->slirp, fwd->server, fwd->port);
}

static void guestfwd_read(void *opaque, const uint8_t *buf, int size)
{
    struct GuestFwd *fwd = opaque;
    slirp_socket_recv(fwd->slirp, fwd->server, fwd->port, buf, size);
}

static ssize_t guestfwd_write(const void *buf, size_t len, void *chr)
{
    return qemu_chr_fe_write_all(chr, buf, len);
}

static int slirp_guestfwd(SlirpState *s, const char *config_str, Error **errp)
{
    /* TODO: IPv6 */
    struct in_addr server = { .s_addr = 0 };
    struct GuestFwd *fwd;
    const char *p;
    char buf[128];
    char *end;
    int port;

    p = config_str;
    if (get_str_sep(buf, sizeof(buf), &p, ':') < 0) {
        goto fail_syntax;
    }
    if (strcmp(buf, "tcp") && buf[0] != '\0') {
        goto fail_syntax;
    }
    if (get_str_sep(buf, sizeof(buf), &p, ':') < 0) {
        goto fail_syntax;
    }
    if (buf[0] != '\0' && !inet_aton(buf, &server)) {
        goto fail_syntax;
    }
    if (get_str_sep(buf, sizeof(buf), &p, '-') < 0) {
        goto fail_syntax;
    }
    port = strtol(buf, &end, 10);
    if (*end != '\0' || port < 1 || port > 65535) {
        goto fail_syntax;
    }

    snprintf(buf, sizeof(buf), "guestfwd.tcp.%d", port);

    if (g_str_has_prefix(p, "cmd:")) {
        if (slirp_add_exec(s->slirp, &p[4], &server, port) < 0) {
            error_setg(errp, "Conflicting/invalid host:port in guest "
                       "forwarding rule '%s'", config_str);
            return -1;
        }
    } else {
        Error *err = NULL;
        /*
         * FIXME: sure we want to support implicit
         * muxed monitors here?
         */
        Chardev *chr = qemu_chr_new_mux_mon(buf, p, NULL);

        if (!chr) {
            error_setg(errp, "Could not open guest forwarding device '%s'",
                       buf);
            return -1;
        }

        fwd = g_new(struct GuestFwd, 1);
        qemu_chr_fe_init(&fwd->hd, chr, &err);
        if (err) {
            error_propagate(errp, err);
            object_unparent(OBJECT(chr));
            g_free(fwd);
            return -1;
        }

        if (slirp_add_guestfwd(s->slirp, guestfwd_write, &fwd->hd,
                               &server, port) < 0) {
            error_setg(errp, "Conflicting/invalid host:port in guest "
                       "forwarding rule '%s'", config_str);
            qemu_chr_fe_deinit(&fwd->hd, true);
            g_free(fwd);
            return -1;
        }
        fwd->server = server;
        fwd->port = port;
        fwd->slirp = s->slirp;

        qemu_chr_fe_set_handlers(&fwd->hd, guestfwd_can_read, guestfwd_read,
                                 NULL, NULL, fwd, NULL, true);
        s->fwd = g_slist_append(s->fwd, fwd);
    }
    return 0;

 fail_syntax:
    error_setg(errp, "Invalid guest forwarding rule '%s'", config_str);
    return -1;
}

void hmp_info_usernet(Monitor *mon, const QDict *qdict)
{
    SlirpState *s;

    QTAILQ_FOREACH(s, &slirp_stacks, entry) {
        int id;
        bool got_hub_id = net_hub_id_for_client(&s->nc, &id) == 0;
        char *info = slirp_connection_info(s->slirp);
        monitor_printf(mon, "Hub %d (%s):\n%s",
                       got_hub_id ? id : -1,
                       s->nc.name, info);
        g_free(info);
    }
}

static void
net_init_slirp_configs(const StringList *fwd, int flags)
{
    while (fwd) {
        struct slirp_config_str *config;

        config = g_malloc0(sizeof(*config));
        pstrcpy(config->str, sizeof(config->str), fwd->value->str);
        config->flags = flags;
        config->next = slirp_configs;
        slirp_configs = config;

        fwd = fwd->next;
    }
}

static const char **slirp_dnssearch(const StringList *dnsname)
{
    const StringList *c = dnsname;
    size_t i = 0, num_opts = 0;
    const char **ret;

    while (c) {
        num_opts++;
        c = c->next;
    }

    if (num_opts == 0) {
        return NULL;
    }

    ret = g_malloc((num_opts + 1) * sizeof(*ret));
    c = dnsname;
    while (c) {
        ret[i++] = c->value->str;
        c = c->next;
    }
    ret[i] = NULL;
    return ret;
}

int net_init_slirp(const Netdev *netdev, const char *name,
                   NetClientState *peer, Error **errp)
{
    struct slirp_config_str *config;
    char *vnet;
    int ret;
    const NetdevUserOptions *user;
    const char **dnssearch;
    bool ipv4 = true, ipv6 = true;

    assert(netdev->type == NET_CLIENT_DRIVER_USER);
    user = &netdev->u.user;

    if ((user->has_ipv6 && user->ipv6 && !user->has_ipv4) ||
        (user->has_ipv4 && !user->ipv4)) {
        ipv4 = 0;
    }
    if ((user->has_ipv4 && user->ipv4 && !user->has_ipv6) ||
        (user->has_ipv6 && !user->ipv6)) {
        ipv6 = 0;
    }

    vnet = user->net ? g_strdup(user->net) :
           user->ip  ? g_strdup_printf("%s/24", user->ip) :
           NULL;

    dnssearch = slirp_dnssearch(user->dnssearch);

    /* all optional fields are initialized to "all bits zero" */

    net_init_slirp_configs(user->hostfwd, SLIRP_CFG_HOSTFWD);
    net_init_slirp_configs(user->guestfwd, 0);

    ret = net_slirp_init(peer, "user", name, user->q_restrict,
                         ipv4, vnet, user->host,
                         ipv6, user->ipv6_prefix, user->ipv6_prefixlen,
                         user->ipv6_host, user->hostname, user->tftp,
                         user->bootfile, user->dhcpstart,
                         user->dns, user->ipv6_dns, user->smb,
                         user->smbserver, dnssearch, user->domainname,
                         user->tftp_server_name, errp);

    while (slirp_configs) {
        config = slirp_configs;
        slirp_configs = config->next;
        g_free(config);
    }

    g_free(vnet);
    g_free(dnssearch);

    return ret;
}
