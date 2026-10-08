#pragma once
/*
 * M32b: IPv6 (RFC 8200) — addresses, routes, neighbour discovery (RFC 4861), stateless address
 * autoconfiguration (RFC 4862), MLDv1 (RFC 2710), ICMPv6 (RFC 4443). See ip6.c.
 * Everything runs under net_mutex like the IPv4 stack.
 */
#include <kernel/net.h>
#include <kernel/string.h>

#define ETH_P_IPV6 0x86DD
#define IPPROTO_HOPOPTS 0
#define IPPROTO_IPV6 41
#define IPPROTO_ROUTING 43
#define IPPROTO_FRAGMENT 44
#define IPPROTO_ICMPV6 58
#define IPPROTO_NONE 59
#define IPPROTO_DSTOPTS 60

struct ip6hdr {
    uint32_t vtc_flow;               /* version 6, traffic class, flow label */
    uint16_t plen;
    uint8_t nxt, hlim;
    uint8_t src[16], dst[16];
};
struct sockaddr_in6_k { uint16_t family, port; uint32_t flowinfo; uint8_t addr[16]; uint32_t scope_id; };

static inline bool ip6_any(const uint8_t *a) { static const uint8_t z[16]; return !memcmp(a, z, 16); }
static inline bool ip6_loopback(const uint8_t *a) { static const uint8_t l[16] = { [15] = 1 }; return !memcmp(a, l, 16); }
static inline bool ip6_multicast(const uint8_t *a) { return a[0] == 0xff; }
static inline bool ip6_linklocal(const uint8_t *a) { return a[0] == 0xfe && (a[1] & 0xc0) == 0x80; }
static inline bool ip6_mc_linkscope(const uint8_t *a) { return a[0] == 0xff && (a[1] & 0xf) <= 2; }
static inline bool ip6_v4mapped(const uint8_t *a) {
    static const uint8_t m[12] = { [10] = 0xff, [11] = 0xff };
    return !memcmp(a, m, 12);
}
static inline bool ip6_eq(const uint8_t *a, const uint8_t *b) { return !memcmp(a, b, 16); }
static inline void ip6_mapped(uint8_t *out, uint32_t v4) {
    memset(out, 0, 10); out[10] = out[11] = 0xff; memcpy(out + 12, &v4, 4);
}
bool ip6_prefix_eq(const uint8_t *a, const uint8_t *b, int plen);
/* needs a scope id: link-local unicast and link/interface-local multicast */
static inline bool ip6_needs_scope(const uint8_t *a) { return ip6_linklocal(a) || ip6_mc_linkscope(a); }

/* addresses (struct inet6_ifaddr flags are the IFA_F_* values) */
#define IFA_F_NODAD 0x02
#define IFA_F_DADFAILED 0x08
#define IFA_F_DEPRECATED 0x20
#define IFA_F_TENTATIVE 0x40
#define IFA_F_PERMANENT 0x80
#define RT_SCOPE_UNIVERSE 0
#define RT_SCOPE_SITE 200
#define RT_SCOPE_LINK 253
#define RT_SCOPE_HOST 254
#define RTPROT_KERNEL 2
#define RTPROT_BOOT 3
#define RTPROT_RA 9
#define FOREVER 0xffffffffu

struct inet6_ifaddr {
    struct list_node node;
    struct netdev *dev;
    uint8_t addr[16];
    int plen, scope;
    unsigned flags;
    uint32_t valid, pref;            /* seconds left (FOREVER) — as of the last dump/update */
    uint64_t tstamp;                 /* when valid/pref were set */
    int dad_left;                    /* DAD probes still to send */
    uint64_t dad_at;
    bool autoconf;
};
struct rt6_info {
    struct list_node node;
    uint8_t dst[16];
    int plen;
    uint8_t gw[16];
    bool has_gw;
    struct netdev *dev;
    int metric;
    uint8_t proto;
    uint64_t expires;                /* 0: never */
};

struct ip6_opts { int hlim; uint8_t tclass; int oif; bool mcloop, ra, dontfrag; };

void ip6_init(void);
void ip6_input(struct netdev *d, struct pkt *p);
/* route to dst (oif 0: any); nexthop and the selected source address; 0 or -errno */
int ip6_route(const uint8_t *dst, int oif, struct netdev **dev, uint8_t *nexthop, uint8_t *src);
/* build the IPv6 header in front of p->data and send; src nullptr/:: selects one; consumes p */
int ip6_output(struct pkt *p, const uint8_t *src, const uint8_t *dst, uint8_t proto, const struct ip6_opts *o);
struct netdev *ip6_dev_for_local(const uint8_t *a);   /* usable (non-tentative) local address */
bool ip6_is_local(const uint8_t *a);
uint32_t csum_pseudo6(const uint8_t *s, const uint8_t *d, uint8_t proto, uint32_t len);
void icmp6_send_error(struct pkt *orig, int type, int code, uint32_t info);   /* orig->nh: IPv6 header */
int ip6_sock_mtu(const uint8_t *dst, int oif);

/* netlink */
void ip6_addr_dump(void (*cb)(void *ctx, const struct inet6_ifaddr *a), void *ctx, int ifindex);
int ip6_addr_add(struct netdev *d, const uint8_t *addr, int plen, unsigned flags, uint32_t pref, uint32_t valid, bool excl);
int ip6_addr_del(struct netdev *d, const uint8_t *addr, int plen);
void ip6_route_dump(void (*cb)(void *ctx, const struct rt6_info *r), void *ctx);
int ip6_route_add(const uint8_t *dst, int plen, const uint8_t *gw, struct netdev *d, int metric, int proto);
int ip6_route_del(const uint8_t *dst, int plen, const uint8_t *gw, struct netdev *d);
void nd_dump(void (*cb)(void *ctx, struct netdev *d, const uint8_t *ip, const uint8_t *mac, int state), void *ctx);
int nd_set(struct netdev *d, const uint8_t *ip, const uint8_t *mac, bool perm);
int nd_del(struct netdev *d, const uint8_t *ip);

/* transports (net_mutex held; p->nh: IPv6 header, p->data: transport header) */
void udp6_input(struct pkt *p);
void udp6_err(const uint8_t *laddr, uint16_t lport, const uint8_t *raddr, uint16_t rport, int err);
/* o: hop limit, traffic class, interface, ...; src: IPV6_PKTINFO source (nullptr: the bound address or selected) */
int udp6_send(struct sock *s, const uint8_t *data, size_t len, const uint8_t *daddr, uint16_t dport,
              const struct ip6_opts *o, const uint8_t *src);
int net_fmt_addr6(char *b, size_t max, const uint8_t *a);   /* /proc/net/{tcp6,udp6}: 4 native-order words */
void sock_addr6(const struct sock *s, bool remote, uint8_t *out);   /* the address as IPv6 (v4-mapped if needed) */
void tcp6_err(const uint8_t *laddr, uint16_t lport, const uint8_t *raddr, uint16_t rport, int err);
void raw6_input(struct pkt *p, int proto);
bool ping6_input(struct pkt *p);

/* /proc/net */
int net_proc_if_inet6(char *buf, size_t max);
int net_proc_ipv6_route(char *buf, size_t max);
int net_proc_snmp6(char *buf, size_t max);
