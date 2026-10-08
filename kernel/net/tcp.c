/*
 * TCP (RFC 793 with the RFC 1122 fixes): the full state machine, Reno/NewReno congestion
 * control (slow start, congestion avoidance, fast retransmit and recovery, RFC 5681/6582),
 * Jacobson/Karels RTO estimation with Karn's rule and exponential backoff (RFC 6298),
 * delayed ACKs, Nagle, the persist timer for zero windows, keepalives, the MSS option,
 * listen backlogs, RST handling and a short TIME_WAIT. No window scaling, SACK or
 * timestamps: windows top out at 64 KiB, plenty for a VM link.
 *
 * Data lives in per-socket rings: snd holds [snd_una, end of queued data), rcv holds received
 * in-order bytes not yet read. Out-of-order segments wait on ->ooo. Everything runs under
 * net_mutex, driven by tcp_input() (net thread), the timers and the socket layer.
 *
 * Lifetime: a socket with a file is freed by the socket layer when the file goes away
 * (tcp_close() first). After close a connection that still has to finish (FIN handshake,
 * TIME_WAIT) becomes an orphan (file == nullptr) and is freed here when it reaches CLOSED.
 * Unaccepted connections hang off their listener (->parent, ->child_node) and are freed with
 * it or by accept() adopting them.
 */
#include <kernel/net.h>
#include <kernel/net6.h>
#include <kernel/kmalloc.h>
#include <kernel/string.h>
#include <kernel/printk.h>
#include <kernel/time.h>
#include <kernel/errno.h>

struct list_node tcp_socks = LIST_INIT(tcp_socks);
uint64_t tcp_stats[8];      /* in segs, out segs, retransmits, bad, resets in, resets out, active opens, passive opens */

#define TH_FIN 0x01
#define TH_SYN 0x02
#define TH_RST 0x04
#define TH_PSH 0x08
#define TH_ACK 0x10
struct tcphdr {
    uint16_t sport, dport;
    uint32_t seq, ack;
    uint8_t off, flags;
    uint16_t window, check, urg;
};

#define MIN_RTO_MS 200u
#define MAX_RTO_MS 60000u
#define INIT_RTO_MS 1000u
#define TIME_WAIT_MS 60000u          /* 2 MSL, as Linux */
#define TW_MAX 4096                  /* more TIME_WAIT sockets than this are closed at once */
#define MAX_RETRIES 12
#define MAX_SYN_RETRIES 6
#define OOO_MAX 1024                 /* segments; bytes are bounded by the receive ring */
#define DELACK_MS 40

int sysctl_tcp_window_scaling = 1, sysctl_tcp_timestamps = 1, sysctl_tcp_sack = 1, sysctl_tcp_fin_timeout = 60;
/* TcpExt (/proc/net/netstat): timeouts, loss probes, lost retransmits, SACK / Reno recoveries, early retransmits */
uint64_t tcp_ext_stats[6];
int sysctl_tcp_tlp = 1, sysctl_tcp_lost_rexmit = 1;     /* M32b loss recovery knobs (0: plain NewReno/SACK) */
static int tw_count;

static inline bool seq_lt(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }
static inline bool seq_le(uint32_t a, uint32_t b) { return (int32_t)(a - b) <= 0; }
static inline bool seq_gt(uint32_t a, uint32_t b) { return (int32_t)(a - b) > 0; }
static inline bool seq_ge(uint32_t a, uint32_t b) { return (int32_t)(a - b) >= 0; }

static uint32_t rcv_wnd(struct sock *s) {
    size_t space = ring_space(&s->rcv), max = (size_t)65535 << (s->ws_ok ? s->rcv_wscale : 0);
    return space > max ? (uint32_t)max : (uint32_t)space;
}
static uint8_t pick_wscale(size_t cap) { uint8_t w = 0; while (w < 14 && (cap >> w) > 65535) w++; return w; }
static uint32_t ts_now(struct sock *s) { return (uint32_t)(time_ns() / NS_MS) + s->ts_offset; }
/* the window to offer: receiver-side silly window avoidance (RFC 1122 4.2.3.3) */
static uint32_t adv_wnd(struct sock *s) {
    uint32_t w = rcv_wnd(s);
    if (w < s->rcv_mss && w < s->rcv.cap / 2) w = 0;
    return w;
}
/* payload per segment: the MSS minus the options every segment carries (RFC 6691) */
static uint32_t mss(struct sock *s) { return (s->snd_mss ? s->snd_mss : 536) - (s->ts_ok ? 12 : 0); }
/* bytes of queued data not yet sent */
static size_t unsent(struct sock *s) {
    if (s->fin_sent) return 0;
    size_t inflight = s->snd_nxt - s->snd_una;
    return s->snd.len > inflight ? s->snd.len - inflight : 0;
}
static bool syn_acked(struct sock *s) { return s->state != TCP_SYN_SENT && s->state != TCP_SYN_RECV; }

/* ------------------------------------------------------------------ teardown */
static void tcp_unlink(struct sock *s) {
    if (s->node.next && s->node.next != &s->node) list_del(&s->node);
    list_init(&s->node);
}
static void tcp_stop_timers(struct sock *s) {
    ntimer_del(&s->t_rexmt); ntimer_del(&s->t_delack); ntimer_del(&s->t_keep);
}

/* the connection is dead: free it if nobody else owns it. Returns true if freed. */
static void send_rst_for(struct sock *s);
static bool tcp_done(struct sock *s) {
    if (s->state == TCP_LISTEN) {
        s->state = TCP_CLOSE;
        list_for_each_safe(it, tmp, &s->children) {
            struct sock *c = list_entry(it, struct sock, child_node);
            if (c->state != TCP_SYN_RECV) send_rst_for(c);
            tcp_done(c);
        }
    }
    if (s->state == TCP_TIME_WAIT) tw_count--;
    s->state = TCP_CLOSE;
    tcp_stop_timers(s);
    tcp_unlink(s);
    pkt_queue_purge(&s->ooo);
    s->ooo_bytes = 0;
    if (s->parent) {                       /* never accepted */
        list_del(&s->child_node);
        s->parent->nchildren--;
        sock_changed(s->parent);
        s->parent = nullptr;
        sock_free(s);
        return true;
    }
    if (!s->file) { sock_free(s); return true; }
    sock_changed(s);
    return false;
}

bool tcp_abort_ret(struct sock *s, int err) {
    if (err) s->err = err;
    return tcp_done(s);
}
void tcp_abort(struct sock *s, int err) { tcp_abort_ret(s, err); }

/* ------------------------------------------------------------------ output */
/* TCP options for a segment: SYN/SYN-ACK negotiate MSS, SACK-permitted, timestamps and the window
 * scale; later segments carry timestamps and, on ACKs, SACK blocks if they fit in 'room' */
static size_t build_opts(struct sock *s, uint8_t *o, uint8_t flags, bool syn, size_t room) {
    size_t n = 0;
    uint32_t tsval = ts_now(s), tsecr = (flags & TH_ACK) ? s->ts_recent : 0;
    if (syn) {
        o[n++] = 2; o[n++] = 4; o[n++] = (uint8_t)(s->rcv_mss >> 8); o[n++] = (uint8_t)s->rcv_mss;
        if (s->ts_ok) {
            if (s->sack_ok) { o[n++] = 4; o[n++] = 2; } else { o[n++] = 1; o[n++] = 1; }
        } else if (s->sack_ok) { o[n++] = 1; o[n++] = 1; o[n++] = 4; o[n++] = 2; }
    } else if (s->ts_ok) { o[n++] = 1; o[n++] = 1; }
    if (s->ts_ok) {
        o[n++] = 8; o[n++] = 10;
        uint32_t a = htonl(tsval), b = htonl(tsecr);
        memcpy(o + n, &a, 4); memcpy(o + n + 4, &b, 4); n += 8;
    }
    if (syn && s->ws_ok) { o[n++] = 1; o[n++] = 3; o[n++] = 3; o[n++] = s->rcv_wscale; }
    if (!syn && s->sack_ok && s->nsack_rcv && (flags & TH_ACK) && room >= n + 12) {
        int k = (int)MIN((size_t)s->nsack_rcv, (MIN(room, (size_t)40) - n - 4) / 8);
        o[n++] = 1; o[n++] = 1; o[n++] = 5; o[n++] = (uint8_t)(2 + 8 * k);
        for (int b = 0; b < k; b++) {
            uint32_t x = htonl(s->sack_rcv[b].start), y = htonl(s->sack_rcv[b].end);
            memcpy(o + n, &x, 4); memcpy(o + n + 4, &y, 4); n += 8;
        }
    }
    return n;
}

/* segment at seq with dlen bytes taken from the send ring at ring offset roff */
static struct pkt *tcp_build(struct sock *s, uint32_t seq, uint8_t flags, size_t roff, size_t dlen, bool syn_opts) {
    uint8_t opts[40];
    uint32_t smss = s->snd_mss ? s->snd_mss : 536;
    size_t optlen = build_opts(s, opts, flags, syn_opts, dlen < smss ? MIN((size_t)40, smss - dlen) : 0);
    struct pkt *p = pkt_alloc(sizeof(struct tcphdr) + optlen + dlen);
    if (!p) return nullptr;
    struct tcphdr *t = (struct tcphdr *)p->data;
    p->len = sizeof *t + optlen + dlen;
    memset(t, 0, sizeof *t);
    t->sport = s->lport; t->dport = s->rport;
    t->seq = htonl(seq);
    if (flags & TH_ACK) t->ack = htonl(s->rcv_nxt);
    t->off = (uint8_t)(((sizeof *t + optlen) / 4) << 4);
    t->flags = flags;
    uint32_t w = adv_wnd(s);
    unsigned sh = (s->ws_ok && !(flags & TH_SYN)) ? s->rcv_wscale : 0;
    bool keep = seq_lt(s->rcv_nxt + w, s->rcv_adv) && s->rcv_adv - s->rcv_nxt <= s->rcv.cap;
    if (keep) w = s->rcv_adv - s->rcv_nxt;                              /* never shrink the window */
    uint32_t field = keep ? (w + (1u << sh) - 1) >> sh : w >> sh;     /* scaled: round down unless keeping */
    /* rounding up must not offer more than the ring holds (Linux has slack in its buffer accounting;
     * the ring has none), so then give back the < 2^wscale bytes of the right edge */
    if ((size_t)field << sh > ring_space(&s->rcv)) field = (uint32_t)(ring_space(&s->rcv) >> sh);
    if (field > 65535) field = 65535;
    t->window = htons((uint16_t)field);
    memcpy(t + 1, opts, optlen);
    if (dlen) ring_get(&s->snd, (uint8_t *)(t + 1) + optlen, dlen, roff, false);
    uint32_t ps = s->v6 ? csum_pseudo6(s->laddr6, s->raddr6, IPPROTO_TCP, (uint32_t)p->len)
                        : csum_pseudo(s->laddr, s->raddr, IPPROTO_TCP, (uint16_t)p->len);
    t->check = htons(csum_fold(csum_partial(t, p->len, ps)));
    if (flags & TH_ACK) {
        s->rcv_adv = s->rcv_nxt + (field << sh);
        s->last_ack_sent = s->rcv_nxt;
        s->delack_segs = 0;
        ntimer_del(&s->t_delack);
    }
    return p;
}

static void tcp_xmit(struct sock *s, struct pkt *p) {
    if (!p) return;
    tcp_stats[1]++;
    if (s->v6) {
        struct ip6_opts o6 = { .hlim = s->hops6, .tclass = (uint8_t)(s->tclass6 > 0 ? s->tclass6 : 0), .oif = s->bound_dev };
        ip6_output(p, s->laddr6, s->raddr6, IPPROTO_TCP, &o6);
        return;
    }
    struct ip_opts o = { (uint8_t)s->ttl, (uint8_t)s->tos, false, s->bound_dev, false, false };
    ip_output(p, s->laddr, s->raddr, IPPROTO_TCP, &o);
}

void tcp_send_ack(struct sock *s) { tcp_xmit(s, tcp_build(s, s->snd_nxt, TH_ACK, 0, 0, false)); }
static void send_syn(struct sock *s) {
    uint8_t f = s->state == TCP_SYN_RECV ? TH_SYN | TH_ACK : TH_SYN;
    tcp_xmit(s, tcp_build(s, s->iss, f, 0, 0, true));
}
static void send_rst_for(struct sock *s) {
    tcp_stats[5]++;
    tcp_xmit(s, tcp_build(s, s->snd_nxt, TH_RST | TH_ACK, 0, 0, false));
}

/* RST in answer to a segment that matched no synchronised connection (RFC 793 p.36) */
static void tcp_reset_reply(struct pkt *p) {
    struct iphdr *ih = (struct iphdr *)p->nh;
    struct ip6hdr *h6 = (struct ip6hdr *)p->nh;
    bool v6 = (p->nh[0] >> 4) == 6;
    struct tcphdr *t = (struct tcphdr *)p->data;
    if (t->flags & TH_RST) return;
    if (v6 ? ip6_multicast(h6->dst) : ih->daddr == INADDR_BROADCAST || ipv4_is_multicast(ih->daddr)) return;
    tcp_stats[5]++;
    struct pkt *q = pkt_alloc(sizeof(struct tcphdr));
    if (!q) return;
    struct tcphdr *r = (struct tcphdr *)q->data;
    q->len = sizeof *r;
    memset(r, 0, sizeof *r);
    r->sport = t->dport; r->dport = t->sport;
    uint32_t dlen = p->len - (t->off >> 4) * 4;
    if (t->flags & TH_ACK) { r->seq = t->ack; r->flags = TH_RST; }
    else {
        r->ack = htonl(ntohl(t->seq) + dlen + !!(t->flags & TH_SYN) + !!(t->flags & TH_FIN));
        r->flags = TH_RST | TH_ACK;
    }
    r->off = 5 << 4;
    if (v6) {
        uint8_t src[16], dst[16];
        memcpy(src, h6->dst, 16); memcpy(dst, h6->src, 16);
        r->check = htons(csum_fold(csum_partial(r, q->len, csum_pseudo6(src, dst, IPPROTO_TCP, (uint32_t)q->len))));
        struct ip6_opts o = { .oif = ip6_needs_scope(dst) && p->dev ? p->dev->index : 0 };
        ip6_output(q, src, dst, IPPROTO_TCP, &o);
        return;
    }
    r->check = htons(csum_fold(csum_partial(r, q->len, csum_pseudo(ih->daddr, ih->saddr, IPPROTO_TCP, (uint16_t)q->len))));
    ip_output(q, ih->daddr, ih->saddr, IPPROTO_TCP, nullptr);
}

/* M32b: tail loss probe (RFC 8985 7): with data outstanding, a probe goes out after
 * PTO = max(2·SRTT, 10 ms) instead of waiting for the (≥ 200 ms) RTO; it resends the oldest
 * segment so a lost retransmission or a lost tail is repaired without collapsing cwnd. One probe
 * per flight (tlp_sent is cleared when an ACK advances snd_una). */
static uint64_t pto_ms(struct sock *s) {
    uint64_t p = MAX(2 * s->srtt_us / 1000, 10ull);
    if (s->snd_nxt - s->snd_una <= mss(s)) p += DELACK_MS;      /* one segment: the peer may delay its ACK */
    return MIN(p, (uint64_t)s->rto_ms);
}
static void rearm_rexmt(struct sock *s) {
    if (s->snd_una != s->snd_nxt) {
        s->persist = false;
        s->tlp_armed = sysctl_tcp_tlp && !s->tlp_sent && s->srtt_us && syn_acked(s) && !s->retries;
        ntimer_mod(&s->t_rexmt, (s->tlp_armed ? pto_ms(s) : s->rto_ms) * NS_MS);
    }
    else if (unsent(s) && !s->snd_wnd) {                          /* zero window: persist */
        if (!s->persist || !s->t_rexmt.active) { s->persist = true; ntimer_mod(&s->t_rexmt, s->rto_ms * NS_MS); }
    } else if (s->state != TCP_TIME_WAIT) { s->persist = false; ntimer_del(&s->t_rexmt); }
}

/* send new data and the FIN as far as the windows allow */
void tcp_output(struct sock *s) {
    if (!syn_acked(s) || s->state == TCP_CLOSE || s->state == TCP_LISTEN || s->state == TCP_TIME_WAIT) return;
    for (;;) {
        /* going back after a timeout: skip what the peer has SACKed (Linux keeps the scoreboard) */
        uint32_t lim = UINT32_MAX, sacked = 0;
        for (int i = 0; i < s->nsack_snd; i++) {
            struct sack_blk b = s->sack_snd[i];
            if (seq_lt(s->snd_nxt, s->snd_max) && seq_ge(s->snd_nxt, b.start) && seq_lt(s->snd_nxt, b.end)) s->snd_nxt = b.end;
            if (seq_lt(b.start, s->snd_nxt)) sacked += (seq_lt(b.end, s->snd_nxt) ? b.end : s->snd_nxt) - b.start;
            else if (seq_lt(s->snd_nxt, s->snd_max) && lim == UINT32_MAX) lim = b.start - s->snd_nxt;
        }
        size_t u = unsent(s);
        if (!u) break;
        uint32_t inflight = s->snd_nxt - s->snd_una;
        uint32_t pipe = inflight - sacked;          /* RFC 6675: SACKed data has left the network */
        uint32_t w = s->cwnd < s->snd_wnd ? s->cwnd : s->snd_wnd;
        uint32_t avail = s->snd_wnd > inflight ? MIN(w > pipe ? w - pipe : 0, s->snd_wnd - inflight) : 0;
        size_t n = MIN(u, (size_t)avail);
        n = MIN(n, (size_t)mss(s));
        n = MIN(n, (size_t)lim);
        if (!n) break;
        /* Nagle (RFC 896): hold back a small segment while data is unacknowledged, unless
         * it is everything there is and FIN follows */
        if (n < mss(s) && inflight && !s->nodelay && !(s->fin_queued && n == u)) break;
        if (n < u && n < mss(s) && n < s->snd_wnd / 2 && inflight) break;   /* sender SWS avoidance */
        struct pkt *p = tcp_build(s, s->snd_nxt, TH_ACK | (n == u ? TH_PSH : 0), inflight, n, false);
        if (!p) break;
        tcp_xmit(s, p);
        if (!s->rtt_timing) { s->rtt_timing = true; s->rtt_seq = s->snd_nxt; s->rtt_start = time_ns(); }
        s->snd_nxt += n;
        if (seq_gt(s->snd_nxt, s->snd_max)) s->snd_max = s->snd_nxt;
    }
    if (s->fin_queued && !s->fin_sent && !unsent(s) &&
        (s->state == TCP_FIN_WAIT1 || s->state == TCP_CLOSING || s->state == TCP_LAST_ACK)) {
        s->fin_seq = s->snd_nxt;
        s->fin_sent = true;
        tcp_xmit(s, tcp_build(s, s->snd_nxt, TH_FIN | TH_ACK, 0, 0, false));
        s->snd_nxt++;
        if (seq_gt(s->snd_nxt, s->snd_max)) s->snd_max = s->snd_nxt;
    }
    rearm_rexmt(s);
}

/* resend the oldest unacknowledged segment (fast retransmit, NewReno partial ACKs) */
static void retransmit_head(struct sock *s) {
    tcp_stats[2]++;
    s->rtt_timing = false;                      /* Karn */
    if (!syn_acked(s)) { send_syn(s); return; }
    size_t n = MIN(s->snd.len, (size_t)mss(s));
    if (s->fin_sent && s->snd_una == s->fin_seq) { tcp_xmit(s, tcp_build(s, s->fin_seq, TH_FIN | TH_ACK, 0, 0, false)); return; }
    if (n) tcp_xmit(s, tcp_build(s, s->snd_una, TH_ACK | TH_PSH, 0, n, false));
    s->head_rtx_ns = time_ns();
    if (n && seq_lt(s->high_rxt, s->snd_una + (uint32_t)n)) s->high_rxt = s->snd_una + (uint32_t)n;
    if (n && seq_lt(s->rtx_high, s->snd_una + (uint32_t)n)) s->rtx_high = s->snd_una + (uint32_t)n;
}

/* ------------------------------------------------------------------ timers */
static void tcp_timer_rexmt(struct ntimer *t) {
    struct sock *s = container_of(t, struct sock, t_rexmt);
    if (s->state == TCP_TIME_WAIT) { tcp_done(s); return; }
    if (s->persist) {                           /* window probe: an old seq elicits an ACK with the window */
        s->persist = false;
        if (unsent(s) && !s->snd_wnd && s->snd_una == s->snd_nxt) {
            if (s->snd.len) {                   /* one byte beyond the window */
                tcp_xmit(s, tcp_build(s, s->snd_nxt, TH_ACK, 0, 1, false));
            } else tcp_xmit(s, tcp_build(s, s->snd_una - 1, TH_ACK, 0, 0, false));
            s->rto_ms = MIN(s->rto_ms * 2, MAX_RTO_MS);
            s->persist = true;
            ntimer_mod(&s->t_rexmt, s->rto_ms * NS_MS);
        } else tcp_output(s);
        return;
    }
    if (s->snd_una == s->snd_nxt) return;
    if (s->tlp_armed) {                         /* probe timeout: resend the head, then wait for the RTO */
        s->tlp_armed = false;
        s->tlp_sent = true;
        tcp_ext_stats[1]++;
        retransmit_head(s);
        uint64_t left = s->rto_ms > pto_ms(s) ? s->rto_ms - pto_ms(s) : 1;
        ntimer_mod(&s->t_rexmt, MAX(left, 10ull) * NS_MS);
        return;
    }
    s->retries++;
    tcp_ext_stats[0]++;
    if (s->retries > (syn_acked(s) ? MAX_RETRIES : MAX_SYN_RETRIES)) { tcp_abort(s, ETIMEDOUT); return; }
    s->rto_ms = MIN(s->rto_ms * 2, MAX_RTO_MS);
    if (!syn_acked(s)) { retransmit_head(s); ntimer_mod(&s->t_rexmt, s->rto_ms * NS_MS); return; }
    /* RFC 5681: ssthresh = max(FlightSize/2, 2*SMSS), cwnd = 1 SMSS, go back N */
    uint32_t flight = s->snd_max - s->snd_una;
    s->ssthresh = MAX(flight / 2, 2 * mss(s));
    s->cwnd = mss(s);
    s->in_recovery = false;
    s->dupacks = 0;
    s->rtt_timing = false;
    tcp_stats[2]++;
    s->snd_nxt = s->snd_una;
    s->high_rxt = s->snd_una;
    s->rtx_high = s->snd_max;                   /* all of it goes out again */
    if (s->fin_sent && seq_le(s->snd_una, s->fin_seq)) s->fin_sent = false;
    tcp_output(s);
    if (!s->t_rexmt.active) ntimer_mod(&s->t_rexmt, s->rto_ms * NS_MS);
}

static void tcp_timer_delack(struct ntimer *t) {
    struct sock *s = container_of(t, struct sock, t_delack);
    if (s->delack_segs && s->state != TCP_CLOSE) tcp_send_ack(s);
}

static void tcp_timer_keep(struct ntimer *t) {
    struct sock *s = container_of(t, struct sock, t_keep);
    uint64_t now = time_ns();
    if (s->state == TCP_FIN_WAIT2 && !s->file) { tcp_done(s); return; }   /* orphan timeout */
    if (!s->keepalive || (s->state != TCP_ESTABLISHED && s->state != TCP_CLOSE_WAIT)) return;
    uint64_t idle = now - s->last_rx;
    if (idle < (uint64_t)s->keepidle * NS_S) { ntimer_mod(t, (uint64_t)s->keepidle * NS_S - idle); return; }
    if (s->keep_probes >= (int)s->keepcnt) { tcp_abort(s, ETIMEDOUT); return; }
    s->keep_probes++;
    tcp_xmit(s, tcp_build(s, s->snd_una - 1, TH_ACK, 0, 0, false));
    ntimer_mod(t, (uint64_t)s->keepintvl * NS_S);
}

void tcp_keepalive_changed(struct sock *s) {
    if (s->keepalive && (s->state == TCP_ESTABLISHED || s->state == TCP_CLOSE_WAIT))
        ntimer_mod(&s->t_keep, (uint64_t)s->keepidle * NS_S);
    else if (s->state != TCP_FIN_WAIT2) ntimer_del(&s->t_keep);
}

/* ------------------------------------------------------------------ socket operations */
void tcp_sock_init(struct sock *s) {
    s->state = TCP_CLOSE;
    s->t_rexmt.fn = tcp_timer_rexmt;
    s->t_delack.fn = tcp_timer_delack;
    s->t_keep.fn = tcp_timer_keep;
    s->rto_ms = INIT_RTO_MS;
    s->keepidle = 7200; s->keepintvl = 75; s->keepcnt = 9;
    list_init(&s->ooo);
    list_init(&s->children);
    list_init(&s->node);
}

static bool tcp_alloc_rings(struct sock *s) {
    if (!s->snd.buf && !ring_alloc(&s->snd, (size_t)s->sndbuf)) return false;
    if (!s->rcv.buf && !ring_alloc(&s->rcv, (size_t)s->rcvbuf)) return false;
    return true;
}

static void set_mss(struct sock *s, struct netdev *d) {
    uint32_t m = s->v6 ? (uint32_t)(d ? d->mtu : 1280) - 60 : (uint32_t)(d ? d->mtu : 576) - 40;
    s->rcv_mss = m > 65495 ? 65495 : m;
    if (!s->snd_mss || s->snd_mss > s->rcv_mss) s->snd_mss = s->rcv_mss;
}

static uint32_t new_iss(void) { return (uint32_t)random_u64(); }

int tcp_connect(struct sock *s) {
    struct netdev *d;
    if (s->v6) {
        uint8_t nh6[16], src6[16];
        int r = ip6_route(s->raddr6, s->bound_dev, &d, nh6, src6);
        if (r) return r;
        if (ip6_any(s->laddr6)) {
            if (ip6_any(src6)) return -EADDRNOTAVAIL;
            memcpy(s->laddr6, src6, 16);
        }
    } else {
        uint32_t nh, src;
        int r = ip_route(s->raddr, s->bound_dev, &d, &nh, &src);
        if (r) return r;
        if (!s->laddr) s->laddr = src;
    }
    if (!tcp_alloc_rings(s)) return -ENOBUFS;
    set_mss(s, d);
    s->snd_mss = 536;                         /* until the peer's MSS option arrives */
    if (d == loopback_dev) s->snd_mss = s->rcv_mss;
    s->iss = new_iss(); s->rtx_high = s->iss;
    s->snd_una = s->iss; s->snd_nxt = s->snd_max = s->iss + 1;
    s->snd_wnd = 1;
    s->ws_ok = sysctl_tcp_window_scaling;
    s->rcv_wscale = s->ws_ok ? pick_wscale(s->rcv.cap) : 0;
    s->snd_wscale = 0;
    s->ts_ok = sysctl_tcp_timestamps;
    s->ts_offset = (uint32_t)random_u64();
    s->ts_recent = 0; s->ts_recent_stamp = 0;
    s->sack_ok = sysctl_tcp_sack;
    s->nsack_rcv = s->nsack_snd = 0;
    s->cwnd = 10 * 536;
    s->ssthresh = 0xffffffff;
    s->retries = 0;
    s->rto_ms = INIT_RTO_MS;
    s->state = TCP_SYN_SENT;
    s->err = 0;
    list_add_tail(&tcp_socks, &s->node);
    tcp_stats[6]++;
    send_syn(s);
    ntimer_mod(&s->t_rexmt, s->rto_ms * NS_MS);
    sock_changed(s);
    return 0;
}

int tcp_listen(struct sock *s, int backlog) {
    if (backlog < 1) backlog = 1;
    if (backlog > 4096) backlog = 4096;
    s->backlog = backlog;
    if (s->state == TCP_LISTEN) return 0;
    if (s->state != TCP_CLOSE) return -EINVAL;
    s->state = TCP_LISTEN;
    list_add_tail(&tcp_socks, &s->node);
    sock_changed(s);
    return 0;
}

/* user close(): RFC 793 CLOSE plus the RFC 2525 rule (unread data => RST) */
void tcp_close(struct sock *s) {
    s->file = nullptr;
    switch (s->state) {
    case TCP_LISTEN: case TCP_CLOSE: case TCP_SYN_SENT:
        tcp_done(s);
        return;
    default: break;
    }
    if (s->rcv.len || (s->linger_on && !s->linger_s)) {
        if (s->state != TCP_TIME_WAIT) send_rst_for(s);
        tcp_done(s);
        return;
    }
    switch (s->state) {
    case TCP_SYN_RECV: case TCP_ESTABLISHED: s->state = TCP_FIN_WAIT1; break;
    case TCP_CLOSE_WAIT: s->state = TCP_LAST_ACK; break;
    case TCP_FIN_WAIT2: ntimer_mod(&s->t_keep, (uint64_t)sysctl_tcp_fin_timeout * NS_S); return;
    case TCP_TIME_WAIT:
        ring_free(&s->snd); ring_free(&s->rcv); pkt_queue_purge(&s->ooo); s->ooo_bytes = 0;
        if (tw_count > TW_MAX) ntimer_mod(&s->t_rexmt, NS_MS);
        return;
    default: return;                         /* FIN_WAIT1, CLOSING, LAST_ACK, TIME_WAIT: in progress */
    }
    s->fin_queued = true;
    tcp_output(s);
}

int tcp_shutdown(struct sock *s, int how) {
    if (how != 0) {                          /* SHUT_WR / SHUT_RDWR */
        if (s->state == TCP_ESTABLISHED) s->state = TCP_FIN_WAIT1;
        else if (s->state == TCP_CLOSE_WAIT) s->state = TCP_LAST_ACK;
        else if (s->state == TCP_SYN_SENT || s->state == TCP_LISTEN) { tcp_done(s); return 0; }
        else return s->state == TCP_CLOSE ? -ENOTCONN : 0;
        s->fin_queued = true;
        tcp_output(s);
    }
    sock_changed(s);
    return 0;
}

/* the reader freed receive space: announce a window that opened by >= 1 MSS or half the ring */
void tcp_recv_window_update(struct sock *s) {
    if (s->state == TCP_CLOSE || s->state == TCP_LISTEN || !syn_acked(s)) return;
    uint32_t w = adv_wnd(s);
    uint32_t adv_left = seq_gt(s->rcv_adv, s->rcv_nxt) ? s->rcv_adv - s->rcv_nxt : 0;
    if (w > adv_left && (w - adv_left >= MIN(s->rcv_mss, (uint32_t)s->rcv.cap / 2) || adv_left == 0)) tcp_send_ack(s);
}

/* ------------------------------------------------------------------ input */
static struct sock *tcp_lookup(struct pkt *p, uint16_t sport, uint16_t dport) {
    struct sock *listener = nullptr;
    struct netdev *dev = p->dev;
    bool v6 = (p->nh[0] >> 4) == 6;
    struct iphdr *ih = (struct iphdr *)p->nh;
    struct ip6hdr *h6 = (struct ip6hdr *)p->nh;
    int lscore = -1;
    list_for_each(it, &tcp_socks) {
        struct sock *s = list_entry(it, struct sock, node);
        if (s->lport != dport) continue;
        if (v6 ? !sock_v6ok(s) : !sock_v4ok(s)) continue;
        if (s->state == TCP_LISTEN) {
            bool spec = v6 ? !ip6_any(s->laddr6) : s->laddr != 0;
            if (spec && (v6 ? !ip6_eq(s->laddr6, h6->dst) : s->laddr != ih->daddr)) continue;
            if (s->bound_dev && dev && dev != loopback_dev && s->bound_dev != dev->index) continue;
            int sc = (spec ? 2 : 0) + (s->family == (v6 ? AF_INET6 : AF_INET) ? 1 : 0);
            if (sc > lscore) { listener = s; lscore = sc; }
            continue;
        }
        if (s->rport != sport) continue;
        if (v6) { if (ip6_eq(s->raddr6, h6->src) && ip6_eq(s->laddr6, h6->dst)) return s; }
        else if (s->raddr == ih->saddr && s->laddr == ih->daddr) return s;
    }
    return listener;
}

struct tcp_opts {
    uint32_t mss, tsval, tsecr;
    int wscale;                        /* -1: absent */
    bool ts, sackperm;
    int nsack;
    struct sack_blk sack[4];
};
static void parse_opts(struct tcphdr *t, struct tcp_opts *r) {
    memset(r, 0, sizeof *r);
    r->wscale = -1;
    uint8_t *o = (uint8_t *)(t + 1), *end = (uint8_t *)t + (t->off >> 4) * 4;
    while (o < end) {
        if (*o == 0) break;
        if (*o == 1) { o++; continue; }
        if (o + 1 >= end || o[1] < 2 || o + o[1] > end) break;
        uint32_t a, b;
        switch (o[0]) {
        case 2: if (o[1] == 4) r->mss = (uint32_t)(o[2] << 8 | o[3]); break;
        case 3: if (o[1] == 3) r->wscale = o[2] > 14 ? 14 : o[2]; break;
        case 4: if (o[1] == 2) r->sackperm = true; break;
        case 5:
            for (int k = 0; k < (o[1] - 2) / 8 && k < 4; k++) {
                memcpy(&a, o + 2 + 8 * k, 4); memcpy(&b, o + 6 + 8 * k, 4);
                r->sack[r->nsack++] = (struct sack_blk){ ntohl(a), ntohl(b) };
            }
            break;
        case 8:
            if (o[1] == 10) { memcpy(&a, o + 2, 4); memcpy(&b, o + 6, 4); r->ts = true; r->tsval = ntohl(a); r->tsecr = ntohl(b); }
            break;
        }
        o += o[1];
    }
}
static void apply_mss(struct sock *s, const struct tcp_opts *o) {
    if (!o->mss) return;
    uint32_t m = o->mss < 64 ? 64 : o->mss;
    s->snd_mss = MIN(m, s->rcv_mss);
}

/* ------------------------------------------------------------------ SACK */
/* receiver: rebuild the blocks to report from the out-of-order queue; the block holding
 * 'recent' (the segment just queued) goes first (RFC 2018 section 4) */
static void sack_rebuild(struct sock *s, uint32_t recent, bool have_recent) {
    struct sack_blk r[OOO_MAX];
    int n = 0;
    list_for_each(it, &s->ooo) {
        struct pkt *q = list_entry(it, struct pkt, node);
        if (seq_le(q->end, s->rcv_nxt)) continue;
        uint32_t st = seq_lt(q->seq, s->rcv_nxt) ? s->rcv_nxt : q->seq;
        if (n && seq_le(st, r[n - 1].end)) { if (seq_gt(q->end, r[n - 1].end)) r[n - 1].end = q->end; }
        else if (n < OOO_MAX) r[n++] = (struct sack_blk){ st, q->end };
    }
    int max = s->ts_ok ? 3 : 4, k = 0;
    int first = -1;
    if (have_recent)
        for (int i = 0; i < n; i++) if (seq_ge(recent, r[i].start) && seq_lt(recent, r[i].end)) { first = i; break; }
    if (first >= 0) s->sack_rcv[k++] = r[first];
    for (int i = n - 1; i >= 0 && k < max; i--) if (i != first) s->sack_rcv[k++] = r[i];   /* newest data is usually highest */
    s->nsack_rcv = k;
}

/* sender: merge the peer's SACK blocks into the scoreboard (sorted, disjoint, above snd_una) */
static void sack_trim(struct sock *s) {
    int k = 0;
    for (int i = 0; i < s->nsack_snd; i++) {
        struct sack_blk b = s->sack_snd[i];
        if (seq_le(b.end, s->snd_una)) continue;
        if (seq_lt(b.start, s->snd_una)) b.start = s->snd_una;
        s->sack_snd[k++] = b;
    }
    s->nsack_snd = k;
}
static void sack_update(struct sock *s, const struct tcp_opts *o) {
    for (int i = 0; i < o->nsack; i++) {
        struct sack_blk b = o->sack[i];
        if (!seq_lt(b.start, b.end) || seq_le(b.end, s->snd_una) || seq_gt(b.end, s->snd_max)) continue;  /* D-SACK / bogus */
        if (seq_lt(b.start, s->snd_una)) b.start = s->snd_una;
        struct sack_blk out[9];
        int n = 0;
        bool placed = false;
        for (int j = 0; j < s->nsack_snd; j++) {
            struct sack_blk c = s->sack_snd[j];
            if (seq_lt(c.end, b.start)) { out[n++] = c; continue; }
            if (seq_gt(c.start, b.end)) { if (!placed) { out[n++] = b; placed = true; } out[n++] = c; continue; }
            if (seq_lt(c.start, b.start)) b.start = c.start;          /* overlap: absorb */
            if (seq_gt(c.end, b.end)) b.end = c.end;
        }
        if (!placed) out[n++] = b;
        if (n > 8) n = 8;                                              /* drop the highest */
        memcpy(s->sack_snd, out, (size_t)n * sizeof *out);
        s->nsack_snd = n;
    }
}
static uint32_t sacked_bytes(struct sock *s) {
    uint32_t n = 0;
    for (int i = 0; i < s->nsack_snd; i++) n += s->sack_snd[i].end - s->sack_snd[i].start;
    return n;
}
/* retransmit the next hole below the highest SACKed byte (one segment); false if none */
static bool sack_retransmit(struct sock *s) {
    if (!s->sack_ok || !s->nsack_snd) return false;
    uint32_t h = seq_gt(s->high_rxt, s->snd_una) ? s->high_rxt : s->snd_una;
    for (int i = 0; i < s->nsack_snd; i++) {
        struct sack_blk b = s->sack_snd[i];
        if (seq_ge(h, b.end)) continue;
        if (seq_ge(h, b.start)) { h = b.end; continue; }
        uint32_t n = MIN(b.start - h, mss(s));
        size_t roff = h - s->snd_una;
        if (roff >= s->snd.len) return false;
        n = (uint32_t)MIN((size_t)n, s->snd.len - roff);
        tcp_stats[2]++;
        tcp_xmit(s, tcp_build(s, h, TH_ACK, roff, n, false));
        if (h == s->snd_una) s->head_rtx_ns = time_ns();
        s->high_rxt = h + n;
        if (seq_lt(s->rtx_high, h + n)) s->rtx_high = h + n;
        return true;
    }
    return false;
}

static void rto_update(struct sock *s) {
    uint64_t rto = (s->srtt_us + MAX(4 * s->rttvar_us, 10000ull)) / 1000;
    s->rto_ms = MAX(MIN(rto, (uint64_t)MAX_RTO_MS), (uint64_t)MIN_RTO_MS);
}
static void rtt_sample(struct sock *s, uint64_t us) {
    if (!us) us = 1;
    if (!s->srtt_us) { s->srtt_us = us; s->rttvar_us = us / 2; }
    else {
        uint64_t d = us > s->srtt_us ? us - s->srtt_us : s->srtt_us - us;
        s->rttvar_us = (3 * s->rttvar_us + d) / 4;
        s->srtt_us = (7 * s->srtt_us + us) / 8;
    }
    rto_update(s);
}

/* the 3-way handshake completed on a passive open */
static void child_established(struct sock *c) {
    c->state = TCP_ESTABLISHED;
    c->was_connected = true;
    c->accepted_ready = true;
    ntimer_del(&c->t_rexmt);
    c->retries = 0;
    if (c->parent) sock_changed(c->parent);
}

static void enter_time_wait(struct sock *s) {
    if (s->state != TCP_TIME_WAIT) tw_count++;
    s->state = TCP_TIME_WAIT;
    if (!s->file) { ring_free(&s->snd); ring_free(&s->rcv); pkt_queue_purge(&s->ooo); s->ooo_bytes = 0; }
    ntimer_del(&s->t_keep);
    ntimer_del(&s->t_delack);
    s->persist = false;
    /* too many TIME_WAIT orphans: expire this one almost at once (Linux "TCP: time wait bucket table overflow") */
    ntimer_mod(&s->t_rexmt, (!s->file && tw_count > TW_MAX ? 1 : TIME_WAIT_MS) * NS_MS);
}

/* process an acceptable ACK; returns false if s was freed */
static bool process_ack(struct sock *s, struct tcphdr *t, uint32_t seq, uint32_t ack, uint32_t wnd, size_t dlen, const struct tcp_opts *o) {
    if (seq_gt(ack, s->snd_max)) {
        /* the ACK covers a zero-window probe byte (sent beyond snd_max): take it */
        if (ack - s->snd_una <= s->snd.len) s->snd_nxt = s->snd_max = ack;
        else { tcp_send_ack(s); return true; }                        /* acks something never sent */
    }       /* acks something never sent */
    if (seq_gt(ack, s->snd_nxt)) s->snd_nxt = ack;                       /* after a go-back-N */
    bool wnd_update = false;
    /* send window update (RFC 793 SND.WL1/WL2 rule) */
    if (seq_lt(s->snd_wl1, seq) || (s->snd_wl1 == seq && seq_le(s->snd_wl2, ack))) {
        wnd_update = wnd != s->snd_wnd;
        s->snd_wnd = wnd; s->snd_wl1 = seq; s->snd_wl2 = ack;
    }
    if (seq_gt(ack, s->snd_una)) {
        uint32_t acked = ack - s->snd_una;
        /* Karn for timestamps too: an ACK for retransmitted data echoes the timestamp of the
         * segment that left the hole (RFC 7323 4.1), which would inflate the RTO */
        bool rtx = seq_lt(s->snd_una, s->rtx_high);
        if (!seq_lt(ack, s->rtx_high)) s->rtx_high = ack;
        if (rtx) s->rtt_timing = false;
        else if (s->ts_ok && o && o->ts && o->tsecr) {                  /* RFC 7323 RTTM */
            uint32_t d = ts_now(s) - o->tsecr;
            if (d < 600000) rtt_sample(s, (uint64_t)d * 1000);
            s->rtt_timing = false;
        } else if (s->rtt_timing && seq_gt(ack, s->rtt_seq)) { s->rtt_timing = false; rtt_sample(s, (time_ns() - s->rtt_start) / 1000); }
        bool fin_acked = s->fin_sent && seq_gt(ack, s->fin_seq);
        uint32_t data = fin_acked ? acked - 1 : acked;
        ring_drop(&s->snd, data);
        s->snd_una = ack;
        s->retries = 0;
        s->tlp_sent = false;
        if (o && s->sack_ok) sack_update(s, o);
        sack_trim(s);
        /* congestion control */
        if (s->in_recovery) {
            if (seq_ge(ack, s->recover)) { s->in_recovery = false; s->cwnd = s->ssthresh; }
            else {                                                       /* NewReno partial ACK */
                if (!sack_retransmit(s)) retransmit_head(s);
                s->cwnd = s->cwnd > data ? s->cwnd - data + mss(s) : mss(s);
            }
        } else if (s->cwnd < s->ssthresh) s->cwnd += MIN(data, mss(s));
        else s->cwnd += MAX(1u, mss(s) * mss(s) / s->cwnd);
        if (s->cwnd > 1u << 20) s->cwnd = 1u << 20;
        s->dupacks = 0;
        if (s->srtt_us) rto_update(s);                                   /* undo the backoff */
        if (fin_acked) {
            if (s->state == TCP_FIN_WAIT1) {
                s->state = TCP_FIN_WAIT2;
                if (!s->file) ntimer_mod(&s->t_keep, (uint64_t)sysctl_tcp_fin_timeout * NS_S);
            } else if (s->state == TCP_CLOSING) enter_time_wait(s);
            else if (s->state == TCP_LAST_ACK) { tcp_done(s); return false; }
        }
        rearm_rexmt(s);
        sock_changed(s);
    } else if (ack == s->snd_una && !dlen && !(t->flags & (TH_SYN | TH_FIN)) && !wnd_update && s->snd_una != s->snd_nxt) {
        /* duplicate ACK; with SACK, also enter recovery once 3 segments above a hole were SACKed */
        if (o && s->sack_ok) sack_update(s, o);
        ++s->dupacks;
        /* early retransmit (RFC 5827): with fewer than 4 segments out and nothing new to send,
         * fewer than 3 duplicate ACKs can ever arrive — use oseg − 1 */
        uint32_t oseg = (s->snd_max - s->snd_una + mss(s) - 1) / mss(s), thresh = 3;
        if (sysctl_tcp_lost_rexmit && oseg >= 2 && oseg < 4 && !unsent(s)) thresh = oseg - 1;
        if (!s->in_recovery && ((uint32_t)s->dupacks >= thresh || (s->sack_ok && sacked_bytes(s) >= 3 * mss(s)))) {
            uint32_t flight = s->snd_max - s->snd_una;
            s->ssthresh = MAX(flight / 2, 2 * mss(s));
            s->recover = s->snd_max;
            s->in_recovery = true;
            tcp_ext_stats[s->sack_ok ? 3 : 4]++;
            if (s->dupacks < 3 && !(s->sack_ok && sacked_bytes(s) >= 3 * mss(s))) tcp_ext_stats[5]++;
            s->high_rxt = s->snd_una;
            s->rtt_timing = false;
            if (!sack_retransmit(s)) retransmit_head(s);
            s->cwnd = s->ssthresh + 3 * mss(s);
        } else if (s->in_recovery) {
            s->cwnd += mss(s);
            /* lost retransmission (RACK-style, RFC 8985): ACKs keep arriving more than
             * SRTT + SRTT/4 after the head was resent, yet it is still unacknowledged — the
             * retransmission was lost too; resend it instead of waiting for the RTO */
            uint64_t now = time_ns();
            if (sysctl_tcp_lost_rexmit && s->srtt_us && s->head_rtx_ns &&
                now - s->head_rtx_ns > (s->srtt_us + s->srtt_us / 4) * 1000 + NS_MS) {
                s->high_rxt = s->snd_una;
                tcp_ext_stats[2]++;
                if (!sack_retransmit(s)) retransmit_head(s);
            } else sack_retransmit(s);                                   /* next hole, one per ACK */
        }
    }
    return true;
}

static void deliver_fin(struct sock *s) {
    s->rcv_nxt++;
    s->peer_fin = true;
    switch (s->state) {
    case TCP_SYN_RECV: case TCP_ESTABLISHED: s->state = TCP_CLOSE_WAIT; break;
    case TCP_FIN_WAIT1: s->state = TCP_CLOSING; break;    /* our FIN not yet acked */
    case TCP_FIN_WAIT2: enter_time_wait(s); break;
    case TCP_TIME_WAIT: enter_time_wait(s); break;        /* retransmitted FIN: restart 2MSL */
    default: break;
    }
}

/* queue a segment that starts beyond rcv_nxt; the queue is kept sorted by seq */
static void ooo_insert(struct sock *s, struct pkt *p, uint32_t seq, uint32_t end) {
    if (s->ooo_bytes + p->len > s->rcv.cap || end == seq) { pkt_free(p); return; }
    p->seq = seq; p->end = end;
    struct list_node *pos = &s->ooo;
    int n = 0;
    list_for_each(it, &s->ooo) {
        struct pkt *q = list_entry(it, struct pkt, node);
        n++;
        if (q->seq == seq && seq_ge(q->end, end)) { pkt_free(p); return; }   /* duplicate */
        if (seq_gt(q->seq, seq)) { pos = it; break; }
    }
    if (n >= OOO_MAX) { pkt_free(p); return; }
    list_add_tail(pos, &p->node);          /* before pos */
    s->ooo_bytes += p->len;
}

/* move queued segments that now connect to rcv_nxt into the ring */
static bool ooo_drain(struct sock *s) {
    bool fin = false;
    list_for_each_safe(it, tmp, &s->ooo) {
        struct pkt *q = list_entry(it, struct pkt, node);
        if (seq_gt(q->seq, s->rcv_nxt)) break;
        list_del(it);
        s->ooo_bytes -= q->len;
        if (seq_gt(q->end, s->rcv_nxt)) {
            uint32_t skip = s->rcv_nxt - q->seq;
            size_t n = ring_put(&s->rcv, q->data + skip, q->len - skip);
            s->rcv_nxt += (uint32_t)n;
            if (q->proto && s->rcv_nxt == q->end) fin = true;    /* proto: FIN rode on this segment */
        }
        pkt_free(q);
    }
    return fin;
}

void tcp_input(struct pkt *p) {
    struct iphdr *ih = (struct iphdr *)p->nh;
    struct ip6hdr *h6 = (struct ip6hdr *)p->nh;
    bool v6 = (p->nh[0] >> 4) == 6;
    struct tcphdr *t = (struct tcphdr *)p->data;
    tcp_stats[0]++;
    if (p->len < sizeof *t || (t->off >> 4) < 5 || (size_t)(t->off >> 4) * 4 > p->len) goto bad;
    uint32_t ps = v6 ? csum_pseudo6(h6->src, h6->dst, IPPROTO_TCP, (uint32_t)p->len) : csum_pseudo(ih->saddr, ih->daddr, IPPROTO_TCP, (uint16_t)p->len);
    if (!p->csum_ok && csum_fold(csum_partial(t, p->len, ps))) goto bad;
    if (v6 ? ip6_multicast(h6->dst) : ih->daddr == INADDR_BROADCAST || ipv4_is_multicast(ih->daddr)) goto drop;
    size_t hl = (t->off >> 4) * 4;
    size_t dlen = p->len - hl;
    uint32_t seq = ntohl(t->seq), ack = ntohl(t->ack), wnd = ntohs(t->window);
    uint8_t fl = t->flags;
    struct tcp_opts o;
    parse_opts(t, &o);
    struct sock *s = tcp_lookup(p, t->sport, t->dport);
    if (!s) { tcp_reset_reply(p); goto drop; }

    /* TIME_WAIT: a new SYN may reuse the pair (RFC 1122 4.2.2.13) */
    if (s->state == TCP_TIME_WAIT && (fl & TH_SYN) && !(fl & TH_ACK) &&
        (seq_gt(seq, s->rcv_nxt) || (s->ts_ok && o.ts && (int32_t)(o.tsval - s->ts_recent) > 0))) {
        tcp_done(s);
        s = tcp_lookup(p, t->sport, t->dport);
        if (!s || s->state != TCP_LISTEN) { if (!s) tcp_reset_reply(p); goto drop; }
    }

    if (s->state == TCP_LISTEN) {
        if (fl & TH_RST) goto drop;
        if (fl & TH_ACK) { tcp_reset_reply(p); goto drop; }
        if (!(fl & TH_SYN)) goto drop;
        if (s->nchildren >= s->backlog + 1) goto drop;        /* the client retries its SYN */
        struct sock *c = sock_new_child(s);
        if (!c) goto drop;
        c->lport = t->dport; c->rport = t->sport;
        struct netdev *d;
        if (v6) {
            c->v6 = true;
            memcpy(c->laddr6, h6->dst, 16); memcpy(c->raddr6, h6->src, 16);
            if (!c->bound_dev && ip6_needs_scope(h6->src) && p->dev && p->dev != loopback_dev) c->bound_dev = p->dev->index;
            uint8_t nh6[16];
            if (ip6_route(c->raddr6, c->bound_dev, &d, nh6, nullptr)) d = nullptr;
        } else {
            c->laddr = ih->daddr; c->raddr = ih->saddr;
            uint32_t nh, src;
            if (ip_route(c->raddr, c->bound_dev, &d, &nh, &src)) d = nullptr;
        }
        set_mss(c, d);
        c->snd_mss = 536;
        apply_mss(c, &o);
        c->ws_ok = sysctl_tcp_window_scaling && o.wscale >= 0;
        c->snd_wscale = c->ws_ok ? (uint8_t)o.wscale : 0;
        c->rcv_wscale = c->ws_ok ? pick_wscale(c->rcv.cap) : 0;
        c->ts_ok = sysctl_tcp_timestamps && o.ts;
        c->ts_offset = (uint32_t)random_u64();
        if (c->ts_ok) { c->ts_recent = o.tsval; c->ts_recent_stamp = time_ns(); }
        c->sack_ok = sysctl_tcp_sack && o.sackperm;
        c->irs = seq; c->rcv_nxt = seq + 1;
        c->iss = new_iss(); c->rtx_high = c->iss;
        c->snd_una = c->iss; c->snd_nxt = c->snd_max = c->iss + 1;
        c->snd_wnd = wnd; c->snd_wl1 = seq; c->snd_wl2 = c->iss;
        c->cwnd = 10 * mss(c); c->ssthresh = 0xffffffff;
        c->state = TCP_SYN_RECV;
        c->rcv_adv = c->rcv_nxt;
        c->last_rx = time_ns();
        list_add_tail(&tcp_socks, &c->node);
        list_add_tail(&s->children, &c->child_node);
        s->nchildren++;
        tcp_stats[7]++;
        send_syn(c);
        ntimer_mod(&c->t_rexmt, c->rto_ms * NS_MS);
        goto drop;
    }

    if (s->state == TCP_SYN_SENT) {
        if ((fl & TH_ACK) && (seq_le(ack, s->iss) || seq_gt(ack, s->snd_max))) {
            if (!(fl & TH_RST)) tcp_reset_reply(p);
            goto drop;
        }
        if (fl & TH_RST) { if (fl & TH_ACK) { tcp_stats[4]++; tcp_abort(s, ECONNREFUSED); } goto drop; }
        if (!(fl & TH_SYN)) goto drop;
        s->irs = seq; s->rcv_nxt = seq + 1;
        s->rcv_adv = s->rcv_nxt;
        apply_mss(s, &o);
        s->ws_ok = s->ws_ok && o.wscale >= 0;
        s->snd_wscale = s->ws_ok ? (uint8_t)o.wscale : 0;
        if (!s->ws_ok) s->rcv_wscale = 0;
        s->ts_ok = s->ts_ok && o.ts;
        if (s->ts_ok) { s->ts_recent = o.tsval; s->ts_recent_stamp = time_ns(); }
        s->sack_ok = s->sack_ok && o.sackperm;
        s->snd_wnd = wnd; s->snd_wl1 = seq; s->snd_wl2 = ack;
        s->last_rx = time_ns();
        if (fl & TH_ACK) {
            s->snd_una = ack;
            if (!s->retries) rtt_sample(s, (time_ns() - (s->t_rexmt.when - s->rto_ms * NS_MS)) / 1000);
            s->rtt_timing = false;
            s->cwnd = 10 * mss(s);
            s->state = TCP_ESTABLISHED;
            s->was_connected = true;
            s->retries = 0;
            ntimer_del(&s->t_rexmt);
            tcp_send_ack(s);
            tcp_keepalive_changed(s);
            tcp_output(s);
            sock_changed(s);
        } else {                                       /* simultaneous open */
            s->state = TCP_SYN_RECV;
            send_syn(s);
        }
        goto drop;
    }

    /* synchronised states */
    if (s->ws_ok) wnd <<= s->snd_wscale;
    /* PAWS (RFC 7323 5.3): an old timestamp marks a stale duplicate */
    if (s->ts_ok && o.ts && !(fl & TH_RST) && s->ts_recent_stamp && (int32_t)(o.tsval - s->ts_recent) < 0 &&
        time_ns() - s->ts_recent_stamp < 24ull * 86400 * NS_S) {
        tcp_send_ack(s);
        goto drop;
    }
    /* segment acceptability (RFC 793 p.69) */
    uint32_t w = rcv_wnd(s);
    uint32_t seg_len = (uint32_t)dlen + !!(fl & TH_SYN) + !!(fl & TH_FIN);
    bool ok;
    if (!seg_len) ok = w ? seq_ge(seq, s->rcv_nxt) && seq_lt(seq, s->rcv_nxt + w) : seq == s->rcv_nxt;
    else ok = w && (seq_ge(seq, s->rcv_nxt) ? seq_lt(seq, s->rcv_nxt + w) : seq_gt(seq + seg_len, s->rcv_nxt));
    if (!ok) {
        if (fl & TH_RST) goto drop;
        /* zero window: still take ACK/window information from in-sequence segments */
        if (!w && seq == s->rcv_nxt && (fl & TH_ACK)) {
            if (!process_ack(s, t, seq, ack, wnd, 0, &o)) goto drop;
            tcp_output(s);
        }
        /* RFC 793 answers with an ACK; for pure ACKs rate-limit it (as Linux does) so two
         * confused ends cannot ping-pong forever */
        uint64_t now = time_ns();
        if (seg_len || now - s->last_oow_ack > 500 * NS_MS) { s->last_oow_ack = now; tcp_send_ack(s); }
        goto drop;
    }
    s->last_rx = time_ns();
    s->keep_probes = 0;
    if (s->ts_ok && o.ts && seq_le(seq, s->last_ack_sent)) { s->ts_recent = o.tsval; s->ts_recent_stamp = s->last_rx; }
    if (fl & TH_RST) {
        tcp_stats[4]++;
        int err = s->state == TCP_SYN_RECV ? ECONNREFUSED : s->state == TCP_CLOSE_WAIT ? EPIPE : ECONNRESET;
        if (s->state == TCP_TIME_WAIT || s->state == TCP_LAST_ACK || s->state == TCP_CLOSING) err = 0;
        tcp_abort(s, err);
        goto drop;
    }
    if (fl & TH_SYN) {                                  /* RFC 5961: challenge ACK */
        if (s->state == TCP_SYN_RECV && seq == s->irs) { send_syn(s); goto drop; }
        tcp_send_ack(s);
        goto drop;
    }
    if (!(fl & TH_ACK)) goto drop;
    if (s->state == TCP_SYN_RECV) {
        if (seq_le(ack, s->iss) || seq_gt(ack, s->snd_max)) { tcp_reset_reply(p); goto drop; }
        s->snd_una = s->iss + 1;
        s->snd_wnd = wnd; s->snd_wl1 = seq; s->snd_wl2 = ack;
        if (!s->retries) rtt_sample(s, (time_ns() - (s->t_rexmt.when - s->rto_ms * NS_MS)) / 1000);
        child_established(s);
        tcp_keepalive_changed(s);
        if (!s->parent && !s->file) { tcp_done(s); goto drop; }
    }
    if (!process_ack(s, t, seq, ack, wnd, dlen, &o)) goto drop;
    if (s->state == TCP_TIME_WAIT && !(fl & TH_FIN)) goto drop;

    /* data */
    bool fin = fl & TH_FIN, ack_now = false;
    uint8_t *data = p->data + hl;
    /* data for a connection the user already closed: nobody will read it (Linux resets) */
    if (dlen && !s->file && !s->parent &&
        (s->state == TCP_FIN_WAIT1 || s->state == TCP_FIN_WAIT2 || s->state == TCP_CLOSING || s->state == TCP_LAST_ACK)) {
        s->rcv_nxt = seq + (uint32_t)dlen;
        send_rst_for(s);
        tcp_done(s);
        goto drop;
    }
    if (dlen || fin) {
        bool can_rx = !s->peer_fin && (s->state == TCP_ESTABLISHED || s->state == TCP_FIN_WAIT1 || s->state == TCP_FIN_WAIT2);
        bool deliver = false;
        if (!can_rx) {                           /* retransmission after the peer's FIN */
            ack_now = true;
            if (fin && s->state == TCP_TIME_WAIT) enter_time_wait(s);   /* restart 2MSL */
        } else if (seq_gt(seq, s->rcv_nxt)) {
            pkt_pull(p, hl);
            p->proto = fin;                          /* remember the FIN */
            ooo_insert(s, p, seq, seq + (uint32_t)dlen);
            p = nullptr;
            if (s->sack_ok) sack_rebuild(s, seq, true);
            tcp_send_ack(s);                         /* duplicate ACK for the sender's fast retransmit */
            sock_changed(s);
            goto drop;
        } else {
            uint32_t skip = s->rcv_nxt - seq;
            if (skip > dlen) skip = (uint32_t)dlen;
            size_t want = dlen - skip;
            if (s->shut_rd && want) s->rcv_nxt += (uint32_t)want;   /* read side shut: discard, as Linux does */
            else if (want) {
                size_t n = ring_put(&s->rcv, data + skip, want);
                s->rcv_nxt += (uint32_t)n;
                if (n < want) ack_now = true;
            }
            deliver = fin && s->rcv_nxt == seq + (uint32_t)dlen;
            if (!list_empty(&s->ooo)) { if (ooo_drain(s)) deliver = true; ack_now = true; s->quickack = 16; if (s->sack_ok) sack_rebuild(s, 0, false); }
            if (dlen && !want) { ack_now = true; s->quickack = 16; }    /* a duplicate: the sender has lost our ACK */
            if (s->quickack) { ack_now = true; s->quickack--; }           /* recovering: no delayed ACKs (Linux quickack) */
            else if (want >= s->rcv_mss - (s->ts_ok ? 12u : 0u)) s->delack_segs++;
            else if (want) ack_now = true;
            if (s->delack_segs >= 2 || fin) ack_now = true;
        }
        if (deliver) deliver_fin(s);
        if (ack_now) tcp_send_ack(s);
        else if (s->delack_segs && !s->t_delack.active) ntimer_mod(&s->t_delack, DELACK_MS * NS_MS);
        sock_changed(s);
    }
    tcp_output(s);
    if (s->state == TCP_CLOSE_WAIT && !s->file && !s->parent) {   /* orphan whose peer finished: close fully */
        s->state = TCP_LAST_ACK; s->fin_queued = true; tcp_output(s);
    }
drop:
    if (p) pkt_free(p);
    return;
bad:
    tcp_stats[3]++;
    pkt_free(p);
}

/* ICMP error for one of our connections */
void tcp_err(uint32_t laddr, uint16_t lport, uint32_t raddr, uint16_t rport, int err) {
    list_for_each(it, &tcp_socks) {
        struct sock *s = list_entry(it, struct sock, node);
        if (s->lport != lport || s->rport != rport || s->raddr != raddr || s->state == TCP_LISTEN || s->v6) continue;
        if (s->state == TCP_SYN_SENT || s->state == TCP_SYN_RECV) tcp_abort(s, err);
        return;
    }
}
void tcp6_err(const uint8_t *laddr, uint16_t lport, const uint8_t *raddr, uint16_t rport, int err) {
    list_for_each(it, &tcp_socks) {
        struct sock *s = list_entry(it, struct sock, node);
        if (!s->v6 || s->lport != lport || s->rport != rport || !ip6_eq(s->raddr6, raddr) || s->state == TCP_LISTEN) continue;
        if (!ip6_eq(s->laddr6, laddr)) continue;
        if (s->state == TCP_SYN_SENT || s->state == TCP_SYN_RECV) tcp_abort(s, err);
        return;
    }
}

static int proc_tcp(char *buf, size_t max, bool v6) {
    mutex_lock(&net_mutex);
    int n = snprintf(buf, max, v6 ? "  sl  local_address                         remote_address                        st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n"
                                  : "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n");
    int i = 0;
    list_for_each(it, &tcp_socks) {
        struct sock *s = list_entry(it, struct sock, node);
        if ((size_t)n + 200 >= max) break;
        if ((s->family == AF_INET6) != v6) continue;
        /* timer kind as Linux shows it: 1 retransmit, 2 keepalive, 3 TIME_WAIT, 4 zero-window probe */
        int tr = 0; uint64_t when = 0, now = time_ns();
        struct ntimer *tm = nullptr;
        if (s->t_rexmt.active) { tm = &s->t_rexmt; tr = s->state == TCP_TIME_WAIT ? 3 : s->persist ? 4 : 1; }
        else if (s->t_keep.active) { tm = &s->t_keep; tr = 2; }
        if (tm && tm->when > now) when = tm->when - now;
        if (v6) {
            uint8_t la[16], ra[16];
            sock_addr6(s, false, la); sock_addr6(s, true, ra);
            n += snprintf(buf + n, max - n, "%4d: ", i++);
            n += net_fmt_addr6(buf + n, max - n, la);
            n += snprintf(buf + n, max - n, ":%04X ", ntohs(s->lport));
            n += net_fmt_addr6(buf + n, max - n, ra);
            n += snprintf(buf + n, max - n, ":%04X ", ntohs(s->rport));
        } else n += snprintf(buf + n, max - n, "%4d: %08X:%04X %08X:%04X ", i++, s->laddr, ntohs(s->lport), s->raddr, ntohs(s->rport));
        n += snprintf(buf + n, max - n, "%02X %08X:%08X %02X:%08X %08X %5u        0 %lu 1 0000000000000000 %lu 4 0 10 -1\n",
                      s->state, (unsigned)s->snd.len, (unsigned)(s->state == TCP_LISTEN ? (unsigned)s->nchildren : s->rcv.len),
                      tr, (unsigned)(when / 10000000), s->retries, s->uid, s->file ? s->ino : 0, s->rto_ms / 10);
    }
    mutex_unlock(&net_mutex);
    return MIN(n, (int)max);
}
int net_proc_tcp(char *buf, size_t max) { return proc_tcp(buf, max, false); }
int net_proc_tcp6(char *buf, size_t max) { return proc_tcp(buf, max, true); }

void tcp_init(void) {}
