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
#define IPPROTO_TCP  6
#define IPPROTO_UDP  17
#define IPPROTO_RAW  255

#define INADDR_ANY       0u
#define INADDR_BROADCAST 0xffffffffu
#define INADDR_LOOPBACK  htonl(0x7f000001)
static inline bool ipv4_is_loopback(uint32_t a) { return (ntohl(a) >> 24) == 127; }
static inline bool ipv4_is_multicast(uint32_t a) { return (ntohl(a) >> 28) == 14; }

/* ------------------------------------------------------------------ packets */
#define PKT_HEADROOM 80        /* virtio hdr + ethernet + IP + TCP with options */

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
#define IFF_RUNNING 0x40
#define IFF_NOARP 0x80
#define IFF_PROMISC 0x100
#define IFF_MULTICAST 0x1000
#define ARPHRD_ETHER 1
#define ARPHRD_LOOPBACK 772

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
    uint64_t rx_packets, rx_bytes, rx_errors, rx_dropped, tx_packets, tx_bytes, tx_errors, tx_dropped, multicast;
};

extern struct mutex net_mutex;
extern struct list_node netdevs;
extern struct netdev *loopback_dev;
struct netdev *netdev_register(const char *name, int type, const uint8_t *hw, int mtu,
                               void (*xmit)(struct netdev *, struct pkt *), void *priv);
struct netdev *netdev_by_index(int idx);
struct netdev *netdev_by_name(const char *name);
/* drivers: hand a received frame (data at the ethernet header) to the stack; any context */
void net_rx(struct netdev *d, struct pkt *p);
bool net_is_local_addr(uint32_t a);              /* one of our addresses (or 127/8) */
struct netdev *net_dev_for_local(uint32_t a);

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
struct ip_opts { uint8_t ttl, tos; bool df; int oif; };
int ip_output(struct pkt *p, uint32_t src, uint32_t dst, uint8_t proto, const struct ip_opts *o);
int ip_output_hdrincl(struct pkt *p, int oif);       /* p->data: a complete IP datagram */
void ip_input(struct netdev *d, struct pkt *p);
void icmp_send_unreach(struct pkt *orig, int type, int code);   /* orig->nh: offending datagram */
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
    void *filter;                      /* SO_ATTACH_FILTER program */
    uint32_t icmp_filter;              /* raw ICMP: ICMP_FILTER type mask */
};

extern struct list_node udp_socks, raw_socks, tcp_socks, packet_socks;
void sock_changed(struct sock *s);    /* recompute pollmask, wake pollers */
unsigned sock_poll_mask(struct sock *s);
uint16_t inet_ephemeral_port(int proto, uint32_t laddr);
bool inet_port_in_use(int proto, uint32_t laddr, uint16_t port, struct sock *self, bool reuse);
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
int net_proc_udp(char *buf, size_t max, bool raw);

/* ring buffers */
size_t ring_space(const struct ring *r);
size_t ring_put(struct ring *r, const uint8_t *src, size_t n);
size_t ring_get(struct ring *r, uint8_t *dst, size_t n, size_t skip, bool consume);
void ring_drop(struct ring *r, size_t n);
bool ring_alloc(struct ring *r, size_t cap);

uint64_t random_u64(void);
