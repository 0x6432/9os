#pragma once
/*
 * M32 networking: packet buffers, network devices, IPv4 (ARP, ICMP, UDP, TCP), AF_INET and
 * AF_PACKET sockets. See HANDOFF "M32: Networking" for the design and the locking rules.
 *
 * Locking in one sentence: all protocol state (devices' addresses, routes, ARP, sockets, TCP
 * control blocks, timers) is guarded by the sleeping net_mutex; drivers hand received frames
 * to the "net" kernel thread through a spinlocked queue, and transmit under net_mutex.
 */
#include <kernel/types.h>
#include <kernel/list.h>
#include <kernel/mutex.h>

static inline uint16_t htons(uint16_t v) { return __builtin_bswap16(v); }
static inline uint16_t ntohs(uint16_t v) { return __builtin_bswap16(v); }
static inline uint32_t htonl(uint32_t v) { return __builtin_bswap32(v); }
static inline uint32_t ntohl(uint32_t v) { return __builtin_bswap32(v); }

#define AF_UNIX   1
#define AF_INET   2
#define AF_INET6  10
#define AF_NETLINK 16
#define AF_PACKET 17

#define ETH_P_IP   0x0800
#define ETH_P_ARP  0x0806
#define ETH_P_ALL  0x0003
#define ETH_HLEN   14
#define ETH_ALEN   6

#define IPPROTO_IP   0
#define IPPROTO_ICMP 1
#define IPPROTO_IGMP 2
#define IPPROTO_TCP  6
#define IPPROTO_UDP  17
#define IPPROTO_RAW  255

#define INADDR_ANY       0u
#define INADDR_BROADCAST 0xffffffffu
#define INADDR_LOOPBACK  htonl(0x7f000001)
static inline bool ipv4_is_loopback(uint32_t a) { return (ntohl(a) >> 24) == 127; }
static inline bool ipv4_is_multicast(uint32_t a) { return (ntohl(a) >> 28) == 14; }

/* ------------------------------------------------------------------ packets */
#define PKT_HEADROOM 96        /* virtio hdr + ethernet + IPv6 + hop-by-hop / IPv4 + options */

struct netdev;
struct pkt {
    struct list_node node;
    struct netdev *dev;
    uint8_t *data;             /* current start */
    size_t len;                /* bytes from data */
    uint8_t *nh, *th;          /* network / transport headers (rx) */
    uint16_t proto;            /* ethertype (host order) */
    uint8_t pkttype;           /* PACKET_HOST / BROADCAST / MULTICAST / OTHERHOST / OUTGOING */
    bool csum_ok;              /* loopback: checksums need no verification */
    uint32_t nexthop;          /* ARP resolution queue */
    uint32_t seq, end;         /* TCP out-of-order queue */
    uint64_t stamp;
    size_t cap;
    uint8_t buf[];
};
#define PACKET_HOST 0
#define PACKET_BROADCAST 1
#define PACKET_MULTICAST 2
#define PACKET_OTHERHOST 3
#define PACKET_OUTGOING 4

struct pkt *pkt_alloc(size_t payload);           /* PKT_HEADROOM + payload, data at the payload */
struct pkt *pkt_alloc_rx(size_t len);            /* frame of len bytes, aligned so the IP header is */
struct pkt *pkt_clone(const struct pkt *p);
void pkt_free(struct pkt *p);
static inline uint8_t *pkt_push(struct pkt *p, size_t n) { p->data -= n; p->len += n; return p->data; }
static inline uint8_t *pkt_pull(struct pkt *p, size_t n) { p->data += n; p->len -= n; return p->data; }
void pkt_queue_purge(struct list_node *q);

/* ------------------------------------------------------------------ devices */
#define IFF_UP 0x1
#define IFF_BROADCAST 0x2
#define IFF_LOOPBACK 0x8
#define IFF_POINTOPOINT 0x10
#define IFF_RUNNING 0x40
#define IFF_NOARP 0x80
#define IFF_PROMISC 0x100
#define IFF_ALLMULTI 0x200
#define IFF_MULTICAST 0x1000
#define ARPHRD_ETHER 1
#define ARPHRD_LOOPBACK 772
#define ARPHRD_NONE 65534

struct netdev {
    struct list_node node;
    char name[16];
    int index, type, mtu, txqlen;
    unsigned flags;
    uint8_t hwaddr[ETH_ALEN];
    uint32_t addr, netmask, bcast;   /* network byte order; addr 0: unconfigured */
    /* transmit a complete ethernet frame; consumes p. Called under net_mutex. */
    void (*xmit)(struct netdev *d, struct pkt *p);
    void *priv;
    struct list_node mcgroups;       /* struct mc_group: IPv4 (IGMP) and IPv6 (MLD) memberships */
    int rs_left, hlim6;              /* IPv6: router solicitations still to send; RA hop limit */
    uint64_t rs_at;
    uint64_t rx_packets, rx_bytes, rx_errors, rx_dropped, tx_packets, tx_bytes, tx_errors, tx_dropped, multicast;
};

extern struct mutex net_mutex;
extern struct list_node netdevs;
extern struct netdev *loopback_dev;
struct netdev *netdev_register(const char *name, int type, const uint8_t *hw, int mtu,
                               void (*xmit)(struct netdev *, struct pkt *), void *priv);
void netdev_unregister(struct netdev *d);        /* net_mutex held; the struct stays allocated */
void tun_init(void);
void netdev_set_flags(struct netdev *d, unsigned flags);   /* SIOCSIFFLAGS semantics */
void net_dev_addr_changed(struct netdev *d);               /* IPv4 address/netmask changed */
void netdev_up_hook(struct netdev *d);                     /* ip6.c: link-local address, RS */
void netdev_down_hook(struct netdev *d);
int route_add(uint32_t dst, uint32_t mask, uint32_t gw, struct netdev *d, int metric, unsigned flags);
int route_del(uint32_t dst, uint32_t mask, uint32_t gw, struct netdev *d, int metric);
void arp_dump(void (*cb)(void *ctx, struct netdev *d, uint32_t ip, const uint8_t *mac, int state), void *ctx);
int arp_set(struct netdev *d, uint32_t ip, const uint8_t *mac, bool perm);
int arp_del(struct netdev *d, uint32_t ip);
int net_if_ioctl(uint64_t cmd, void *uarg);   /* SIOC* interface/route/ARP ioctls */
struct netdev *netdev_by_index(int idx);
struct netdev *netdev_by_name(const char *name);
/* drivers: hand a received frame (data at the ethernet header) to the stack; any context */
void net_rx(struct netdev *d, struct pkt *p);
bool net_is_local_addr(uint32_t a);              /* one of our addresses (or 127/8) */
struct netdev *net_dev_for_local(uint32_t a);

/* ------------------------------------------------------------------ multicast (mcast.c) */
struct mc_group {
    struct list_node node;
    uint8_t addr[16];                /* IPv4: first 4 bytes */
    bool v6, reporter;
    int users;
    uint64_t report_at;              /* pending (solicited or repeated) report, 0: none */
};
struct sock;
int mc_dev_join(struct netdev *d, const void *grp, bool v6);
int mc_dev_leave(struct netdev *d, const void *grp, bool v6);
bool mc_dev_has(struct netdev *d, const void *grp, bool v6);
bool mc_mac_ok(struct netdev *d, const uint8_t *mac);
void mc_dev_flush(struct netdev *d);
void mc_query(struct netdev *d, const void *grp, bool v6, uint64_t max_ns);   /* grp nullptr: general */
void mc_heard_report(struct netdev *d, const void *grp, bool v6);
void igmp_input(struct pkt *p);
int sock_mc_join(struct sock *s, const void *grp, bool v6, int ifindex);
int sock_mc_leave(struct sock *s, const void *grp, bool v6, int ifindex);
void sock_mc_drop_all(struct sock *s);
bool sock_mc_allowed(struct sock *s, const void *grp, bool v6, int ifindex);
int net_proc_igmp(char *buf, size_t max, bool v6);
void mld_send(struct netdev *d, const uint8_t *grp, int type);   /* ip6.c: 131 report, 132 done */

/* ------------------------------------------------------------------ timers (net_mutex held) */
struct ntimer {
    struct list_node node;
    uint64_t when;
    void (*fn)(struct ntimer *t);
    bool active;
};
void ntimer_mod(struct ntimer *t, uint64_t delay_ns);
void ntimer_del(struct ntimer *t);
#define NS_MS 1000000ull
#define NS_S  1000000000ull

/* ------------------------------------------------------------------ IPv4 */
struct iphdr {
    uint8_t ver_ihl, tos;
    uint16_t tot_len, id, frag_off;
    uint8_t ttl, protocol;
    uint16_t check;
    uint32_t saddr, daddr;
};
#define IP_DF 0x4000
#define IP_MF 0x2000
#define IP_OFFMASK 0x1fff

struct route {
    struct list_node node;
    uint32_t dst, mask, gw;            /* network order */
    struct netdev *dev;
    int metric;
    unsigned flags;                    /* RTF_* */
};
#define RTF_UP 0x1
#define RTF_GATEWAY 0x2
#define RTF_HOST 0x4
extern struct list_node routes;
/* output route for dst: device, next hop and the preferred source address */
int ip_route(uint32_t dst, int oif, struct netdev **dev, uint32_t *nexthop, uint32_t *src);
void route_add_connected(struct netdev *d);
void route_flush_dev(struct netdev *d);

uint32_t csum_partial(const void *buf, size_t len, uint32_t sum);
uint16_t csum_fold(uint32_t sum);
uint32_t csum_pseudo(uint32_t s, uint32_t d, uint8_t proto, uint16_t len);

/* build the IP header in front of p->data (the transport payload) and send; consumes p */
struct ip_opts { uint8_t ttl, tos; bool df; int oif; bool mcloop, ra; };   /* ra: Router Alert option */
int ip_output(struct pkt *p, uint32_t src, uint32_t dst, uint8_t proto, const struct ip_opts *o);
int ip_output_hdrincl(struct pkt *p, int oif);       /* p->data: a complete IP datagram */
void ip_input(struct netdev *d, struct pkt *p);
void icmp_send_unreach(struct pkt *orig, int type, int code);
void icmp_send_unreach_mtu(struct pkt *orig, uint16_t mtu);   /* orig->nh: offending datagram */
/* ARP / ethernet */
void eth_output(struct netdev *d, struct pkt *p, uint32_t nexthop, uint16_t ethertype);
void arp_input(struct netdev *d, struct pkt *p);
void arp_init(void);
void arp_flush_dev(struct netdev *d);
int net_proc_arp(char *buf, size_t max);

/* ------------------------------------------------------------------ sockets */
struct msghdr_k;
struct iovec_k { void *base; size_t len; };
struct msghdr_k { void *name; uint32_t namelen, _p0; struct iovec_k *iov; size_t iovlen; void *control; size_t controllen; int flags, _p1; };
struct sockaddr_in_k { uint16_t family, port; uint32_t addr; uint8_t zero[8]; };
struct sockaddr_ll_k { uint16_t family, protocol; int32_t ifindex; uint16_t hatype; uint8_t pkttype, halen; uint8_t addr[8]; };

/* TCP states (numbering of /proc/net/tcp) */
enum { TCP_ESTABLISHED = 1, TCP_SYN_SENT, TCP_SYN_RECV, TCP_FIN_WAIT1, TCP_FIN_WAIT2, TCP_TIME_WAIT,
       TCP_CLOSE, TCP_CLOSE_WAIT, TCP_LAST_ACK, TCP_LISTEN, TCP_CLOSING };

struct ring { uint8_t *buf; size_t cap, head, len; };   /* bytes [head, head+len) mod cap */

struct sock {
    int family, type, protocol;
    struct list_node node;             /* protocol table */
    struct file *file;                 /* nullptr: orphaned (TCP after close) or unaccepted */
    uint32_t laddr, raddr;             /* network order */
    uint16_t lport, rport;             /* network order */
    bool connected;                    /* UDP/raw: default destination set */
    bool bound;                        /* explicit or automatic port binding */
    int err;                           /* pending SO_ERROR */
    bool reuseaddr, reuseport, broadcast, keepalive, nodelay, hdrincl, shut_rd, shut_wr, linger_on;
    bool recverr, pktinfo, cork;
    int linger_s, rcvbuf, sndbuf, ttl, tos, bound_dev, mark, priority;
    uint64_t rcvtimeo, sndtimeo;       /* ns, 0 = forever */
    struct list_node rxq;              /* datagram/raw/packet receive queue */
    size_t rxbytes;
    volatile unsigned pollmask;        /* recomputed under net_mutex by sock_changed() */
    uint32_t uid;
    uint64_t ino;
    /* AF_PACKET */
    uint16_t pproto; int ifindex;
    /* TCP */
    int state;
    struct sock *parent;               /* embryonic/unaccepted connection: its listener */
    struct list_node children;         /* listener: SYN_RECV + established, not yet accepted */
    struct list_node child_node;
    int backlog, nchildren;
    bool accepted_ready;               /* child: established and waiting in accept queue */
    uint32_t iss, irs, snd_una, snd_nxt, snd_max, snd_wnd, snd_wl1, snd_wl2, rcv_nxt, rcv_adv;
    uint32_t snd_mss, rcv_mss, cwnd, ssthresh, recover;
    int dupacks;
    bool in_recovery, fin_queued, fin_sent, peer_fin, rtt_timing, ack_pending_now, persist, was_connected;
    uint32_t fin_seq;
    int delack_segs, retries, probes, keep_probes;
    uint32_t rtt_seq;
    uint64_t rtt_start, srtt_us, rttvar_us, rto_ms;
    unsigned keepidle, keepintvl, keepcnt;  /* seconds / count */
    struct ring snd, rcv;
    struct list_node ooo;              /* out-of-order segments (pkt with seq/end) */
    size_t ooo_bytes;
    struct ntimer t_rexmt, t_delack, t_keep;
    uint64_t last_rx;
    uint64_t last_oow_ack;              /* rate limit for ACKs to out-of-window pure ACKs */
    /* TCP options: RFC 7323 window scaling + timestamps, RFC 2018 SACK */
    uint8_t snd_wscale, rcv_wscale;
    bool ws_ok, ts_ok, sack_ok;
    uint32_t ts_recent, ts_offset, last_ack_sent;
    uint64_t ts_recent_stamp;          /* 0: no timestamp seen yet */
    struct sack_blk { uint32_t start, end; } sack_rcv[4];   /* blocks we report, most recent first */
    int nsack_rcv;
    struct sack_blk sack_snd[8];       /* scoreboard: what the peer reported, sorted, above snd_una */
    int nsack_snd;
    uint32_t high_rxt;                 /* SACK loss recovery: holes retransmitted below this */
    int quickack;                      /* ACK the next segments at once (after loss) */
    uint32_t rtx_high;                 /* no RTT samples for ACKs below this (retransmitted) */
    uint64_t head_rtx_ns;              /* when the segment at snd_una was last retransmitted */
    bool tlp_armed, tlp_sent;          /* tail loss probe pending / sent in this flight */
    void *filter;                      /* SO_ATTACH_FILTER program */
    uint32_t icmp_filter;              /* raw ICMP: ICMP_FILTER type mask */
    /* multicast (IP_MULTICAST_*, IP_ADD_MEMBERSHIP; IPV6_* equivalents) */
    uint8_t mc_ttl;
    bool mc_loop, mc_all;
    int mc_ifindex;                    /* IP_MULTICAST_IF / IPV6_MULTICAST_IF */
    uint32_t mc_addr;                  /* IP_MULTICAST_IF by address */
    struct sock_mc { uint8_t grp[16]; bool v6; int ifindex; } mc[20];
    int nmc;
    /* AF_NETLINK */
    uint32_t nl_pid, nl_groups, nl_dst_pid, nl_dst_groups;
    /* AF_INET6: v6 = the socket's addresses are IPv6 ones (else it is unbound/wildcard or speaks
     * IPv4 through v4-mapped addresses in laddr/raddr) */
    uint8_t laddr6[16], raddr6[16];
    bool v6, v6only, rx_pktinfo6, rx_hlim6, rx_2292pktinfo, rx_2292hlim, rx_tclass, dontfrag, rx_ttl;
    int hops6, mc_hops6, tclass6;      /* -1: default */
    int raw_csum;                      /* IPV6_CHECKSUM offset, -1: none */
    uint32_t icmp6_filter[8];          /* ICMP6_FILTER: set bits block */
};
/* may this socket see IPv4 / IPv6 traffic? */
static inline bool sock_v4ok(const struct sock *s) {
    if (s->family == AF_INET) return true;
    return s->family == AF_INET6 && !s->v6 && !s->v6only && s->type != 3 && !(s->type == 2 && s->protocol == 58);
}
static inline bool sock_v6ok(const struct sock *s) { return s->family == AF_INET6 && (s->v6 || (!s->laddr && !s->raddr)); }
int net_proc_tcp6(char *buf, size_t max);
int net_proc_udp6(char *buf, size_t max, bool raw);
int net_proc_netlink(char *buf, size_t max);
extern struct list_node netlink_socks;
int netlink_rcv(struct sock *s, const uint8_t *buf, size_t len);   /* a request was sent */
int netlink_bind(struct sock *s, uint32_t pid, uint32_t groups);
void netlink_autobind(struct sock *s);

extern struct list_node udp_socks, raw_socks, tcp_socks, packet_socks;
void sock_changed(struct sock *s);    /* recompute pollmask, wake pollers */
unsigned sock_poll_mask(struct sock *s);
/* what a binding covers: IPv4 (address a4, 0 = any) and/or IPv6 (a6, :: = any) */
struct bindid { bool v4, v6; uint32_t a4; uint8_t a6[16]; };
uint16_t inet_ephemeral_port(int proto, const struct bindid *b);
bool inet_port_in_use(int proto, const struct bindid *b, uint16_t port, struct sock *self, bool reuse);
void sock_queue_rx(struct sock *s, struct pkt *p);   /* datagram queue (respects rcvbuf) */
void sock_free(struct sock *s);

/* protocol entry points (net_mutex held) */
void udp_input(struct pkt *p);
void udp_err(uint32_t laddr, uint16_t lport, uint32_t raddr, uint16_t rport, int err);
int udp_send(struct sock *s, const uint8_t *data, size_t len, uint32_t daddr, uint16_t dport);
void tcp_input(struct pkt *p);
void tcp_err(uint32_t laddr, uint16_t lport, uint32_t raddr, uint16_t rport, int err);
void raw_input(struct pkt *p);          /* copies to raw sockets (IP header included) */
bool ping_input(struct pkt *p);         /* echo replies for SOCK_DGRAM ICMP sockets */
void packet_input(struct netdev *d, struct pkt *p, bool outgoing);
void tcp_init(void);
void tcp_sock_init(struct sock *s);
void tcp_keepalive_changed(struct sock *s);
struct sock *sock_new_child(struct sock *listener);   /* socket layer */

/* TCP socket operations (net_mutex held) */
int tcp_connect(struct sock *s);
int tcp_listen(struct sock *s, int backlog);
void tcp_close(struct sock *s);         /* the file is going away */
int tcp_shutdown(struct sock *s, int how);
void tcp_output(struct sock *s);
void tcp_send_ack(struct sock *s);
void tcp_recv_window_update(struct sock *s);
void tcp_abort(struct sock *s, int err);
int net_proc_tcp(char *buf, size_t max);
extern int sysctl_ip_forward, sysctl_ip_default_ttl, sysctl_somaxconn, sysctl_ipv6_forwarding, sysctl_ipv6_disable;
extern int sysctl_ipv6_hop_limit, sysctl_ipv6_accept_ra, sysctl_ipv6_dad_transmits, sysctl_ipv6_autoconf, sysctl_icmpv6_echo_ignore_all;
extern int sysctl_tcp_window_scaling, sysctl_tcp_timestamps, sysctl_tcp_sack, sysctl_tcp_fin_timeout;
void ring_free(struct ring *r);
int net_proc_udp(char *buf, size_t max, bool raw);

/* ring buffers */
size_t ring_space(const struct ring *r);
size_t ring_put(struct ring *r, const uint8_t *src, size_t n);
size_t ring_get(struct ring *r, uint8_t *dst, size_t n, size_t skip, bool consume);
void ring_drop(struct ring *r, size_t n);
bool ring_alloc(struct ring *r, size_t cap);

uint64_t random_u64(void);
