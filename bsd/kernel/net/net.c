/* The AuxoBSD IPv4 stack: Ethernet, ARP, IPv4, ICMP echo, UDP, DHCP, DNS and a TCP client.
 *
 * One lock covers all of it. Whoever holds the lock may poll the card; blocking calls take the
 * lock, poll, release it and wait a tick, so frames keep flowing no matter which thread waits.
 * The "net" thread brings the link up (DHCP) and then polls in the background. */
#include "kern.h"
#include "arch.h"
#include "rs.h"
#include "pci.h"
#include "net.h"
#include "vfs.h"

/* ── byte order and checksums ───────────────────────────────────────── */
static inline uint16_t htons(uint16_t v) { return (uint16_t)(v << 8 | v >> 8); }
static inline uint32_t htonl(uint32_t v) { return __builtin_bswap32(v); }
#define ntohs htons
#define ntohl htonl

static uint32_t csum_add(uint32_t sum, const void *data, size_t len)
{
    const uint8_t *p = data;
    for (; len > 1; len -= 2, p += 2) sum += (uint32_t)p[0] << 8 | p[1];
    if (len) sum += (uint32_t)p[0] << 8;
    return sum;
}
static uint16_t csum_fold(uint32_t sum)
{
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return htons((uint16_t)~sum);
}

/* ── state ──────────────────────────────────────────────────────────── */
static struct nic nic;
static int have_nic, ready;
static uint32_t my_ip, my_mask, my_gw, my_dns;
static char status_line[128] = "no network card found";
static volatile int lock;
static uint8_t frame_buf[2048], out_buf[2048];

static const uint8_t BCAST[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static void net_lock(void) { while (__atomic_exchange_n(&lock, 1, __ATOMIC_ACQUIRE)) rs_yield(); }
static void net_unlock(void) { __atomic_store_n(&lock, 0, __ATOMIC_RELEASE); }
static uint32_t now_ms(void) { return (uint32_t)(kernel_ticks() * (1000 / AUXO_HZ)); }

int net_present(void) { return have_nic; }
int net_ready(void) { return ready; }
const struct nic *net_nic(void) { return have_nic ? &nic : 0; }
const char *net_status(void) { return status_line; }
uint32_t net_ip(void) { return my_ip; }
uint32_t net_mask(void) { return my_mask; }
uint32_t net_gateway(void) { return my_gw; }
uint32_t net_dns(void) { return my_dns; }

void ip_fmt(uint32_t ip, char *b, size_t n)
{
    ksnprintf(b, n, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
}

int ip_parse(const char *s, uint32_t *ip)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        if (*s < '0' || *s > '9') return -1;
        uint32_t o = 0;
        while (*s >= '0' && *s <= '9') { o = o * 10 + (uint32_t)(*s++ - '0'); if (o > 255) return -1; }
        v = v << 8 | o;
        if (i < 3 && *s++ != '.') return -1;
    }
    if (*s) return -1;
    *ip = v;
    return 0;
}

/* ── Ethernet + ARP ─────────────────────────────────────────────────── */
struct eth { uint8_t dst[6], src[6]; uint16_t type; } __attribute__((packed));
struct arp {
    uint16_t htype, ptype; uint8_t hlen, plen; uint16_t op;
    uint8_t sha[6]; uint32_t spa; uint8_t tha[6]; uint32_t tpa;
} __attribute__((packed));

#define NARP 16
static struct { uint32_t ip; uint8_t mac[6]; uint32_t when; } arpc[NARP];

volatile uint64_t net_rx_bytes, net_tx_bytes, net_rx_packets, net_tx_packets;   /* for System Monitor */

static int eth_send(const uint8_t *dst, uint16_t type, const void *payload, size_t len)
{
    if (len + sizeof(struct eth) > sizeof out_buf) return -1;
    struct eth *e = (struct eth *)out_buf;
    memcpy(e->dst, dst, 6);
    memcpy(e->src, nic.mac, 6);
    e->type = htons(type);
    memcpy(out_buf + sizeof *e, payload, len);
    int r = nic.send(&nic, out_buf, sizeof *e + len);
    if (r == 0) { net_tx_bytes += sizeof *e + len; net_tx_packets++; }
    return r;
}

static void arp_learn(uint32_t ip, const uint8_t *mac)
{
    int slot = 0;
    for (int i = 0; i < NARP; i++) {
        if (arpc[i].ip == ip) { slot = i; break; }
        if (arpc[i].when < arpc[slot].when) slot = i;
    }
    arpc[slot].ip = ip;
    memcpy(arpc[slot].mac, mac, 6);
    arpc[slot].when = now_ms() | 1;
}

static const uint8_t *arp_lookup(uint32_t ip)
{
    for (int i = 0; i < NARP; i++) if (arpc[i].ip == ip && arpc[i].when) return arpc[i].mac;
    return 0;
}

static void arp_request(uint32_t ip)
{
    struct arp a = { htons(1), htons(0x0800), 6, 4, htons(1), { 0 }, htonl(my_ip), { 0 }, htonl(ip) };
    memcpy(a.sha, nic.mac, 6);
    eth_send(BCAST, 0x0806, &a, sizeof a);
}

static void arp_input(const struct arp *a)
{
    if (ntohs(a->ptype) != 0x0800) return;
    uint32_t spa = ntohl(a->spa), tpa = ntohl(a->tpa);
    if (spa) arp_learn(spa, a->sha);
    if (ntohs(a->op) == 1 && my_ip && tpa == my_ip) {
        struct arp r = { htons(1), htons(0x0800), 6, 4, htons(2), { 0 }, htonl(my_ip), { 0 }, a->spa };
        memcpy(r.sha, nic.mac, 6);
        memcpy(r.tha, a->sha, 6);
        eth_send(a->sha, 0x0806, &r, sizeof r);
    }
}

/* ── IPv4 ───────────────────────────────────────────────────────────── */
struct ip4 {
    uint8_t vhl, tos; uint16_t len, id, frag; uint8_t ttl, proto; uint16_t csum;
    uint32_t src, dst;
} __attribute__((packed));

static uint16_t ip_id = 1;
static void poll_locked(void);

/* may_wait: called from an API path (not from inside packet processing), so it may poll for
 * the ARP reply */
static int ip_send(uint32_t dst, uint8_t proto, const void *payload, size_t len, int may_wait)
{
    uint8_t pkt[1600];
    if (len + sizeof(struct ip4) > sizeof pkt) return -1;
    const uint8_t *mac;
    if (dst == 0xFFFFFFFF) mac = BCAST;
    else {
        uint32_t hop = (my_mask && (dst & my_mask) != (my_ip & my_mask) && my_gw) ? my_gw : dst;
        mac = arp_lookup(hop);
        for (int tries = 0; !mac && may_wait && tries < 3; tries++) {
            arp_request(hop);
            for (uint32_t until = now_ms() + 400; !mac && (int32_t)(now_ms() - until) < 0; ) {
                poll_locked();
                mac = arp_lookup(hop);
                if (!mac) { net_unlock(); wait_tick(); net_lock(); }
            }
        }
        if (!mac) { if (!may_wait) arp_request(hop); return -1; }
    }
    struct ip4 *h = (struct ip4 *)pkt;
    h->vhl = 0x45; h->tos = 0; h->len = htons((uint16_t)(sizeof *h + len)); h->id = htons(ip_id++);
    h->frag = htons(0x4000); h->ttl = 64; h->proto = proto; h->csum = 0;
    h->src = htonl(my_ip); h->dst = htonl(dst);
    h->csum = csum_fold(csum_add(0, h, sizeof *h));
    memcpy(pkt + sizeof *h, payload, len);
    return eth_send(mac, 0x0800, pkt, sizeof *h + len);
}

static uint32_t pseudo(uint32_t src, uint32_t dst, uint8_t proto, size_t len)
{
    uint32_t s = 0;
    s += src >> 16; s += src & 0xFFFF; s += dst >> 16; s += dst & 0xFFFF;
    s += proto; s += (uint32_t)len;
    return s;
}

/* ── ICMP echo ──────────────────────────────────────────────────────── */
struct icmp { uint8_t type, code; uint16_t csum, id, seq; } __attribute__((packed));
static volatile int ping_seq = -1;
static volatile uint32_t ping_from;

static void icmp_input(uint32_t src, const uint8_t *p, size_t len)
{
    if (len < sizeof(struct icmp)) return;
    const struct icmp *ic = (const struct icmp *)p;
    if (ic->type == 8) {
        uint8_t r[1500];
        if (len > sizeof r) return;
        memcpy(r, p, len);
        struct icmp *o = (struct icmp *)r;
        o->type = 0; o->csum = 0;
        o->csum = csum_fold(csum_add(0, r, len));
        ip_send(src, 1, r, len, 0);
    } else if (ic->type == 0 && ntohs(ic->id) == 0xA0B0 && ntohs(ic->seq) == ping_seq && src == ping_from) {
        ping_seq = -1;
    }
}

int net_ping(uint32_t ip, uint16_t seq, int timeout_ms)
{
    if (!ready) return -1;
    uint8_t p[sizeof(struct icmp) + 32];
    struct icmp *ic = (struct icmp *)p;
    ic->type = 8; ic->code = 0; ic->csum = 0; ic->id = htons(0xA0B0); ic->seq = htons(seq);
    for (int i = 0; i < 32; i++) p[sizeof *ic + i] = (uint8_t)('a' + i % 23);
    ic->csum = csum_fold(csum_add(0, p, sizeof p));
    net_lock();
    ping_from = ip;
    ping_seq = seq;
    uint32_t t0 = now_ms();
    if (ip_send(ip, 1, p, sizeof p, 1) < 0) { ping_seq = -1; net_unlock(); return -1; }
    int rtt = -1;
    while ((int32_t)(now_ms() - t0) < timeout_ms) {
        poll_locked();
        if (ping_seq == -1) { rtt = (int)(now_ms() - t0); break; }
        net_unlock(); wait_tick(); net_lock();
    }
    ping_seq = -1;
    net_unlock();
    return rtt;
}

/* ── UDP (DHCP + DNS only: one waiting reply at a time) ─────────────── */
struct udp { uint16_t sport, dport, len, csum; } __attribute__((packed));
static uint16_t udp_wait_port;
static uint8_t udp_reply[1500];
static volatile int udp_reply_len;

static int udp_send(uint32_t dst, uint16_t sport, uint16_t dport, const void *data, size_t len)
{
    uint8_t p[1500];
    if (len + sizeof(struct udp) > sizeof p) return -1;
    struct udp *u = (struct udp *)p;
    u->sport = htons(sport); u->dport = htons(dport); u->len = htons((uint16_t)(sizeof *u + len)); u->csum = 0;
    memcpy(p + sizeof *u, data, len);
    uint32_t s = pseudo(my_ip, dst, 17, sizeof *u + len);
    u->csum = csum_fold(csum_add(s, p, sizeof *u + len));
    if (!u->csum) u->csum = 0xFFFF;
    return ip_send(dst, 17, p, sizeof *u + len, 1);
}

/* UDP sockets for programs (DNS lookups by glibc, mostly): a small queue of datagrams each */
#define NUSK 16
#define UQ 8
struct usk {
    int used, connected;
    uint16_t lport, rport;
    uint32_t rip;
    int qh, qn;
    struct { uint32_t ip; uint16_t port, len; uint8_t data[1472]; } q[UQ];
};
static struct usk usks[NUSK];
static uint16_t next_uport = 33000;
static uint32_t cur_src;                     /* source address of the datagram being handled */

static void udp_input(const uint8_t *p, size_t len)
{
    if (len < sizeof(struct udp)) return;
    const struct udp *u = (const struct udp *)p;
    for (int i = 0; i < NUSK; i++) {
        struct usk *k = &usks[i];
        if (!k->used || k->lport != ntohs(u->dport)) continue;
        if (k->connected && (k->rip != cur_src || k->rport != ntohs(u->sport))) continue;
        if (k->qn == UQ) return;
        size_t n = len - sizeof *u;
        if (n > sizeof k->q[0].data) n = sizeof k->q[0].data;
        int slot = (k->qh + k->qn) % UQ;
        k->q[slot].ip = cur_src; k->q[slot].port = ntohs(u->sport); k->q[slot].len = (uint16_t)n;
        memcpy(k->q[slot].data, p + sizeof *u, n);
        k->qn++;
        return;
    }
    if (!udp_wait_port || ntohs(u->dport) != udp_wait_port || udp_reply_len) return;
    size_t n = len - sizeof *u;
    if (n > sizeof udp_reply) n = sizeof udp_reply;
    memcpy(udp_reply, p + sizeof *u, n);
    udp_reply_len = (int)n;
}

/* send and wait for one reply on sport; returns reply length, -1 on timeout. Lock held. */
static int udp_exchange(uint32_t dst, uint16_t sport, uint16_t dport, const void *q, size_t qlen, int timeout_ms,
                        int (*accept)(const uint8_t *, int))
{
    udp_wait_port = sport;
    udp_reply_len = 0;
    if (udp_send(dst, sport, dport, q, qlen) < 0) { udp_wait_port = 0; return -1; }
    for (uint32_t t0 = now_ms(); (int32_t)(now_ms() - t0) < timeout_ms; ) {
        poll_locked();
        if (udp_reply_len) {
            if (!accept || accept(udp_reply, udp_reply_len)) { udp_wait_port = 0; return udp_reply_len; }
            udp_reply_len = 0;
        }
        net_unlock(); wait_tick(); net_lock();
    }
    udp_wait_port = 0;
    return -1;
}

/* ── DHCP ───────────────────────────────────────────────────────────── */
struct dhcp {
    uint8_t op, htype, hlen, hops; uint32_t xid; uint16_t secs, flags;
    uint32_t ciaddr, yiaddr, siaddr, giaddr; uint8_t chaddr[16], sname[64], file[128];
    uint32_t magic; uint8_t opts[312];
} __attribute__((packed));
static uint32_t dhcp_xid;

static size_t dhcp_build(struct dhcp *d, int type, uint32_t req_ip, uint32_t server)
{
    memset(d, 0, sizeof *d);
    d->op = 1; d->htype = 1; d->hlen = 6; d->xid = htonl(dhcp_xid); d->flags = htons(0x8000);
    memcpy(d->chaddr, nic.mac, 6);
    d->magic = htonl(0x63825363);
    uint8_t *o = d->opts;
    *o++ = 53; *o++ = 1; *o++ = (uint8_t)type;
    *o++ = 61; *o++ = 7; *o++ = 1; memcpy(o, nic.mac, 6); o += 6;
    *o++ = 12; *o++ = 7; memcpy(o, "auxobsd", 7); o += 7;
    if (req_ip) { *o++ = 50; *o++ = 4; uint32_t v = htonl(req_ip); memcpy(o, &v, 4); o += 4; }
    if (server) { *o++ = 54; *o++ = 4; uint32_t v = htonl(server); memcpy(o, &v, 4); o += 4; }
    *o++ = 55; *o++ = 3; *o++ = 1; *o++ = 3; *o++ = 6;          /* mask, router, DNS */
    *o++ = 255;
    return (size_t)(o - (uint8_t *)d);
}

static int dhcp_opt(const struct dhcp *d, int len, int code, uint8_t *out, int cap)
{
    const uint8_t *o = d->opts, *end = (const uint8_t *)d + len;
    while (o < end && *o != 255) {
        if (*o == 0) { o++; continue; }
        if (o + 1 >= end) break;
        int c = o[0], l = o[1];
        if (o + 2 + l > end) break;
        if (c == code) { int n = l < cap ? l : cap; memcpy(out, o + 2, (size_t)n); return n; }
        o += 2 + l;
    }
    return -1;
}

static int dhcp_accept(const uint8_t *p, int len)
{
    const struct dhcp *d = (const struct dhcp *)p;
    return len >= 240 && d->op == 2 && ntohl(d->xid) == dhcp_xid && !memcmp(d->chaddr, nic.mac, 6);
}

static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static int dhcp_run(void)
{
    struct dhcp q;
    dhcp_xid = (uint32_t)rdtsc() ^ 0x41755830;
    my_ip = 0;
    size_t n = dhcp_build(&q, 1, 0, 0);
    int len = udp_exchange(0xFFFFFFFF, 68, 67, &q, n, 3000, dhcp_accept);
    if (len < 0) return -1;
    const struct dhcp *offer = (const struct dhcp *)udp_reply;
    uint8_t v[4], t = 0;
    if (dhcp_opt(offer, len, 53, &t, 1) != 1 || t != 2) return -1;
    uint32_t yi = ntohl(offer->yiaddr), server = dhcp_opt(offer, len, 54, v, 4) == 4 ? get32(v) : 0;
    n = dhcp_build(&q, 3, yi, server);
    len = udp_exchange(0xFFFFFFFF, 68, 67, &q, n, 3000, dhcp_accept);
    if (len < 0) return -1;
    const struct dhcp *ack = (const struct dhcp *)udp_reply;
    if (dhcp_opt(ack, len, 53, &t, 1) != 1 || t != 5) return -1;
    my_ip = ntohl(ack->yiaddr);
    my_mask = dhcp_opt(ack, len, 1, v, 4) == 4 ? get32(v) : 0xFFFFFF00;
    my_gw = dhcp_opt(ack, len, 3, v, 4) == 4 ? get32(v) : 0;
    my_dns = dhcp_opt(ack, len, 6, v, 4) == 4 ? get32(v) : my_gw;
    return 0;
}

/* ── DNS ────────────────────────────────────────────────────────────── */
#define NDNS 16
static struct { char name[64]; uint32_t ip, when; } dnsc[NDNS];
static uint16_t dns_id;

static int dns_accept(const uint8_t *p, int len) { return len >= 12 && (p[0] << 8 | p[1]) == dns_id; }

static int dns_query(const char *host, uint32_t *ip, int timeout_ms)
{
    uint8_t q[300];
    size_t o = 12;
    dns_id = (uint16_t)rdtsc();
    memset(q, 0, 12);
    q[0] = (uint8_t)(dns_id >> 8); q[1] = (uint8_t)dns_id; q[2] = 1; q[5] = 1;      /* RD, 1 question */
    for (const char *s = host; *s; ) {
        const char *dot = strchr(s, '.');
        size_t l = dot ? (size_t)(dot - s) : strlen(s);
        if (!l || l > 63 || o + l + 6 > sizeof q) return -1;
        q[o++] = (uint8_t)l;
        memcpy(q + o, s, l); o += l;
        s += l; if (*s == '.') s++;
    }
    q[o++] = 0; q[o++] = 0; q[o++] = 1; q[o++] = 0; q[o++] = 1;               /* A, IN */
    for (int attempt = 0; attempt < 2; attempt++) {
        int len = udp_exchange(my_dns, (uint16_t)(40000 + (dns_id & 0x3FFF)), 53, q, o, timeout_ms / 2, dns_accept);
        if (len < 0) continue;
        const uint8_t *p = udp_reply;
        if ((p[3] & 0x0F) != 0) return -1;                                     /* NXDOMAIN etc */
        int qd = p[4] << 8 | p[5], an = p[6] << 8 | p[7];
        int i = 12;
        for (int k = 0; k < qd; k++) {                                        /* skip questions */
            while (i < len && p[i]) { if ((p[i] & 0xC0) == 0xC0) { i++; break; } i += p[i] + 1; }
            i += 5;
        }
        for (int k = 0; k < an && i + 10 < len; k++) {
            while (i < len && p[i]) { if ((p[i] & 0xC0) == 0xC0) { i++; break; } i += p[i] + 1; }
            i++;
            int type = p[i] << 8 | p[i + 1], rdl = p[i + 8] << 8 | p[i + 9];
            i += 10;
            if (type == 1 && rdl == 4 && i + 4 <= len) { *ip = get32(p + i); return 0; }
            i += rdl;
        }
        return -1;
    }
    return -1;
}

int net_resolve(const char *host, uint32_t *ip, int timeout_ms)
{
    if (ip_parse(host, ip) == 0) return 0;
    if (!ready || strlen(host) >= 64) return -1;
    for (int i = 0; i < NDNS; i++)
        if (dnsc[i].when && !strcmp(dnsc[i].name, host) && now_ms() - dnsc[i].when < 300000) { *ip = dnsc[i].ip; return 0; }
    net_lock();
    int r = dns_query(host, ip, timeout_ms);
    net_unlock();
    if (r == 0) {
        int slot = 0;
        for (int i = 0; i < NDNS; i++) if (dnsc[i].when < dnsc[slot].when) slot = i;
        strlcpy(dnsc[slot].name, host, sizeof dnsc[slot].name);
        dnsc[slot].ip = *ip;
        dnsc[slot].when = now_ms() | 1;
    }
    return r;
}

/* ── TCP ────────────────────────────────────────────────────────────── */
struct tcph {
    uint16_t sport, dport; uint32_t seq, ack; uint8_t off, flags; uint16_t win, csum, urg;
} __attribute__((packed));
#define FIN 0x01
#define SYN 0x02
#define RST 0x04
#define PSH 0x08
#define ACK 0x10

enum { T_FREE, T_SYN_SENT, T_EST, T_FIN_WAIT, T_CLOSE_WAIT, T_DONE, T_RESET };
#define NSOCK 32
#define RBUF (64 * 1024)
#define SBUF (16 * 1024)

struct tcb {
    int state, peer_fin;
    uint32_t rip; uint16_t lport, rport;
    uint32_t iss, snd_una, snd_nxt, rcv_nxt, peer_wnd, mss;
    uint8_t rbuf[RBUF]; uint32_t rhead, rlen;       /* received, not yet read */
    uint8_t sbuf[SBUF]; uint32_t slen;              /* unacknowledged + unsent, from snd_una */
    uint32_t rto, rto_at, tries, last_wnd;
    int fin_queued, fin_sent;
};
static struct tcb socks[NSOCK];
static uint16_t next_port = 49152;

static uint32_t rwnd(const struct tcb *t) { uint32_t f = RBUF - t->rlen; return f > 65535 ? 65535 : f; }

static int tcp_out(struct tcb *t, uint8_t flags, uint32_t seq, const uint8_t *data, size_t len)
{
    uint8_t p[1600];
    struct tcph *h = (struct tcph *)p;
    size_t hl = sizeof *h + ((flags & SYN) ? 4 : 0);
    h->sport = htons(t->lport); h->dport = htons(t->rport);
    h->seq = htonl(seq); h->ack = htonl(t->rcv_nxt);
    h->off = (uint8_t)((hl / 4) << 4); h->flags = flags;
    h->win = htons((uint16_t)rwnd(t)); h->csum = 0; h->urg = 0;
    if (flags & SYN) { p[20] = 2; p[21] = 4; p[22] = 1460 >> 8; p[23] = 1460 & 255; }
    if (len) memcpy(p + hl, data, len);
    uint32_t s = pseudo(my_ip, t->rip, 6, hl + len);
    h->csum = csum_fold(csum_add(s, p, hl + len));
    t->last_wnd = rwnd(t);
    return ip_send(t->rip, 6, p, hl + len, 0);
}

static void tcp_push(struct tcb *t)
{
    /* send whatever the peer's window allows, from snd_nxt */
    for (;;) {
        uint32_t sent = t->snd_nxt - t->snd_una, wnd = t->peer_wnd ? t->peer_wnd : 1;
        if (sent >= t->slen || sent >= wnd) break;
        uint32_t n = t->slen - sent;
        if (n > t->mss) n = t->mss;
        if (n > wnd - sent) n = wnd - sent;
        tcp_out(t, ACK | PSH, t->snd_nxt, t->sbuf + sent, n);
        t->snd_nxt += n;
        if (!t->rto_at) t->rto_at = now_ms() + t->rto;
    }
    if (t->fin_queued && !t->fin_sent && t->snd_nxt - t->snd_una == t->slen) {
        tcp_out(t, ACK | FIN, t->snd_nxt, 0, 0);
        t->snd_nxt++;
        t->fin_sent = 1;
        if (!t->rto_at) t->rto_at = now_ms() + t->rto;
    }
}

static void tcp_input(uint32_t src, const uint8_t *p, size_t len)
{
    if (len < sizeof(struct tcph)) return;
    const struct tcph *h = (const struct tcph *)p;
    size_t hl = (size_t)(h->off >> 4) * 4;
    if (hl < 20 || hl > len) return;
    struct tcb *t = 0;
    for (int i = 0; i < NSOCK; i++)
        if (socks[i].state != T_FREE && socks[i].rip == src && socks[i].lport == ntohs(h->dport) &&
            socks[i].rport == ntohs(h->sport)) { t = &socks[i]; break; }
    if (!t) return;
    uint32_t seq = ntohl(h->seq), ack = ntohl(h->ack);
    const uint8_t *data = p + hl;
    size_t dlen = len - hl;
    if (h->flags & RST) { t->state = T_RESET; return; }

    if (t->state == T_SYN_SENT) {
        if ((h->flags & (SYN | ACK)) == (SYN | ACK) && ack == t->iss + 1) {
            t->rcv_nxt = seq + 1;
            t->snd_una = t->snd_nxt = ack;
            t->peer_wnd = ntohs(h->win);
            for (size_t i = 20; i + 1 < hl; ) {                 /* MSS option */
                uint8_t k = p[i];
                if (k == 0) break;
                if (k == 1) { i++; continue; }
                if (k == 2 && i + 3 < hl) { uint32_t m = (uint32_t)p[i + 2] << 8 | p[i + 3]; if (m && m < t->mss) t->mss = m; }
                if (p[i + 1] < 2) break;
                i += p[i + 1];
            }
            t->state = T_EST;
            t->rto_at = 0; t->tries = 0;
            tcp_out(t, ACK, t->snd_nxt, 0, 0);
            tcp_push(t);
        }
        return;
    }

    if (h->flags & ACK) {
        uint32_t acked = ack - t->snd_una;
        if ((int32_t)(ack - t->snd_una) > 0 && (int32_t)(ack - t->snd_nxt) <= 0) {
            uint32_t data_acked = acked;
            if (t->fin_sent && ack == t->snd_nxt) data_acked = acked - 1;
            if (data_acked > t->slen) data_acked = t->slen;
            memmove(t->sbuf, t->sbuf + data_acked, t->slen - data_acked);
            t->slen -= data_acked;
            t->snd_una = ack;
            t->tries = 0;
            t->rto = 500;
            t->rto_at = t->snd_una == t->snd_nxt ? 0 : now_ms() + t->rto;
        }
        t->peer_wnd = ntohs(h->win);
    }

    int need_ack = 0;
    if (dlen) {
        if (seq == t->rcv_nxt) {
            uint32_t room = RBUF - t->rlen, n = dlen < room ? (uint32_t)dlen : room;
            for (uint32_t i = 0; i < n; i++) t->rbuf[(t->rhead + t->rlen + i) % RBUF] = data[i];
            t->rlen += n;
            t->rcv_nxt += n;
            if (n < dlen) dlen = n;                              /* rest will be resent */
        }
        need_ack = 1;
    }
    if ((h->flags & FIN) && seq + dlen == t->rcv_nxt && !t->peer_fin) {
        t->rcv_nxt++;
        t->peer_fin = 1;
        need_ack = 1;
        if (t->state == T_EST) t->state = T_CLOSE_WAIT;
    }
    if (need_ack) tcp_out(t, ACK, t->snd_nxt, 0, 0);
    if (t->fin_sent && t->snd_una == t->snd_nxt && t->peer_fin) t->state = T_DONE;
    tcp_push(t);
}

static void tcp_timers(void)
{
    uint32_t now = now_ms();
    for (int i = 0; i < NSOCK; i++) {
        struct tcb *t = &socks[i];
        if (t->state == T_FREE || t->state == T_RESET || t->state == T_DONE || !t->rto_at) continue;
        if ((int32_t)(now - t->rto_at) < 0) continue;
        if (++t->tries > 8) { t->state = T_RESET; continue; }
        t->rto = t->rto < 8000 ? t->rto * 2 : 8000;
        t->rto_at = now + t->rto;
        if (t->state == T_SYN_SENT) { tcp_out(t, SYN, t->iss, 0, 0); continue; }
        t->snd_nxt = t->snd_una;                                 /* go back N */
        t->fin_sent = 0;
        tcp_push(t);
    }
}

int tcp_connect(uint32_t ip, uint16_t port, int timeout_ms)
{
    if (!ready) return -1;
    net_lock();
    int s = -1;
    for (int i = 0; i < NSOCK; i++) if (socks[i].state == T_FREE) { s = i; break; }
    if (s < 0) { net_unlock(); return -1; }
    struct tcb *t = &socks[s];
    memset(t, 0, sizeof *t);
    t->state = T_SYN_SENT;
    t->rip = ip; t->rport = port;
    t->lport = next_port++;
    if (next_port < 49152) next_port = 49152;
    t->iss = (uint32_t)rdtsc();
    t->snd_una = t->snd_nxt = t->iss + 1;
    t->rcv_nxt = 0; t->rhead = t->rlen = 0; t->slen = 0;
    t->mss = 1460; t->peer_wnd = 0; t->rto = 1000; t->tries = 0;
    t->fin_queued = t->fin_sent = 0;
    /* resolve the next hop (may poll), then SYN */
    uint32_t hop = (my_mask && (ip & my_mask) != (my_ip & my_mask) && my_gw) ? my_gw : ip;
    for (int k = 0; k < 3 && !arp_lookup(hop); k++) {
        arp_request(hop);
        for (uint32_t until = now_ms() + 500; !arp_lookup(hop) && (int32_t)(now_ms() - until) < 0; ) {
            poll_locked(); if (!arp_lookup(hop)) { net_unlock(); wait_tick(); net_lock(); }
        }
    }
    tcp_out(t, SYN, t->iss, 0, 0);
    t->rto_at = now_ms() + t->rto;
    for (uint32_t t0 = now_ms(); t->state == T_SYN_SENT && (int32_t)(now_ms() - t0) < timeout_ms; ) {
        poll_locked();
        if (t->state != T_SYN_SENT) break;
        net_unlock(); wait_tick(); net_lock();
    }
    if (t->state != T_EST) { t->state = T_FREE; net_unlock(); return -1; }
    net_unlock();
    return s;
}

int tcp_send(int s, const void *buf, size_t len)
{
    if (s < 0 || s >= NSOCK) return -1;
    struct tcb *t = &socks[s];
    const uint8_t *b = buf;
    size_t done = 0;
    net_lock();
    while (done < len) {
        if (t->state != T_EST && t->state != T_CLOSE_WAIT) { net_unlock(); return -1; }
        uint32_t room = SBUF - t->slen;
        if (room) {
            uint32_t n = len - done < room ? (uint32_t)(len - done) : room;
            memcpy(t->sbuf + t->slen, b + done, n);
            t->slen += n;
            done += n;
            tcp_push(t);
            continue;
        }
        poll_locked();
        net_unlock(); wait_tick(); net_lock();
    }
    net_unlock();
    return (int)done;
}

int tcp_recv(int s, void *buf, size_t cap, int timeout_ms)
{
    if (s < 0 || s >= NSOCK) return -1;
    struct tcb *t = &socks[s];
    net_lock();
    for (uint32_t t0 = now_ms(); ; ) {
        poll_locked();
        if (t->rlen) {
            uint32_t n = t->rlen < cap ? t->rlen : (uint32_t)cap;
            for (uint32_t i = 0; i < n; i++) ((uint8_t *)buf)[i] = t->rbuf[(t->rhead + i) % RBUF];
            t->rhead = (t->rhead + n) % RBUF;
            t->rlen -= n;
            if (t->last_wnd < t->mss * 2 && rwnd(t) >= t->mss * 2) tcp_out(t, ACK, t->snd_nxt, 0, 0);  /* window update */
            net_unlock();
            return (int)n;
        }
        if (t->peer_fin) { net_unlock(); return 0; }
        if (t->state == T_RESET || t->state == T_FREE) { net_unlock(); return -1; }
        if ((int32_t)(now_ms() - t0) >= timeout_ms) { net_unlock(); return -2; }
        net_unlock(); wait_tick(); net_lock();
    }
}

void tcp_close(int s)
{
    if (s < 0 || s >= NSOCK) return;
    struct tcb *t = &socks[s];
    net_lock();
    if (t->state == T_EST || t->state == T_CLOSE_WAIT) {
        t->fin_queued = 1;
        tcp_push(t);
        /* give the FIN a moment, without waiting for the full close handshake */
        for (uint32_t t0 = now_ms(); t->state != T_DONE && t->state != T_RESET && (int32_t)(now_ms() - t0) < 300; ) {
            poll_locked(); net_unlock(); wait_tick(); net_lock();
        }
        if (t->state != T_DONE && t->state != T_RESET) tcp_out(t, RST | ACK, t->snd_nxt, 0, 0);
    }
    t->state = T_FREE;
    net_unlock();
}

/* ── for sockets in Linux programs (sys.c) ──────────────────────────── */
int tcp_readable(int s)
{
    if (s < 0 || s >= NSOCK) return 1;
    struct tcb *t = &socks[s];
    return t->rlen || t->peer_fin || t->state == T_RESET || t->state == T_FREE;
}

int tcp_writable(int s)
{
    if (s < 0 || s >= NSOCK) return 0;
    struct tcb *t = &socks[s];
    return (t->state == T_EST || t->state == T_CLOSE_WAIT) && t->slen < SBUF && !t->fin_queued;
}

int tcp_hup(int s)
{
    if (s < 0 || s >= NSOCK) return 1;
    struct tcb *t = &socks[s];
    return t->state == T_RESET || t->state == T_FREE || (t->peer_fin && t->fin_queued);
}

void tcp_shutdown(int s)
{
    if (s < 0 || s >= NSOCK) return;
    struct tcb *t = &socks[s];
    net_lock();
    if (t->state == T_EST || t->state == T_CLOSE_WAIT) { t->fin_queued = 1; tcp_push(t); }
    net_unlock();
}

void tcp_endpoints(int s, uint32_t *lip, uint16_t *lport, uint32_t *rip, uint16_t *rport)
{
    struct tcb *t = &socks[s < 0 || s >= NSOCK ? 0 : s];
    *lip = my_ip; *lport = t->lport; *rip = t->rip; *rport = t->rport;
}

int udp_open(void)
{
    net_lock();
    for (int i = 0; i < NUSK; i++)
        if (!usks[i].used) {
            memset(&usks[i], 0, sizeof usks[i]);
            usks[i].used = 1;
            net_unlock();
            return i;
        }
    net_unlock();
    return -1;
}

static int uport_free(uint16_t p)
{
    for (int i = 0; i < NUSK; i++) if (usks[i].used && usks[i].lport == p) return 0;
    return p != 68;
}

int udp_bind(int s, uint16_t port)
{
    if (s < 0 || s >= NUSK || !usks[s].used) return -1;
    net_lock();
    if (!port) {
        for (int k = 0; k < 20000 && !port; k++) {
            uint16_t c = next_uport++;
            if (next_uport > 60999) next_uport = 33000;
            if (uport_free(c)) port = c;
        }
    } else if (!uport_free(port)) { net_unlock(); return -2; }
    usks[s].lport = port;
    net_unlock();
    return port ? 0 : -1;
}

int udp_connect(int s, uint32_t ip, uint16_t port)
{
    if (s < 0 || s >= NUSK || !usks[s].used) return -1;
    if (!usks[s].lport && udp_bind(s, 0) < 0) return -1;
    usks[s].rip = ip; usks[s].rport = port; usks[s].connected = ip != 0;
    return 0;
}

int udp_sendto(int s, uint32_t ip, uint16_t port, const void *buf, size_t len)
{
    if (s < 0 || s >= NUSK || !usks[s].used || !ready) return -1;
    if (!usks[s].lport && udp_bind(s, 0) < 0) return -1;
    if (len > 1472) return -2;
    net_lock();
    int r = udp_send(ip, usks[s].lport, port, buf, len);
    net_unlock();
    return r < 0 ? -1 : (int)len;
}

/* one datagram, or -2 when none is waiting */
int udp_recvfrom(int s, void *buf, size_t cap, uint32_t *ip, uint16_t *port)
{
    if (s < 0 || s >= NUSK || !usks[s].used) return -1;
    net_lock();
    poll_locked();
    struct usk *k = &usks[s];
    if (!k->qn) { net_unlock(); return -2; }
    size_t n = k->q[k->qh].len < cap ? k->q[k->qh].len : cap;
    memcpy(buf, k->q[k->qh].data, n);
    if (ip) *ip = k->q[k->qh].ip;
    if (port) *port = k->q[k->qh].port;
    k->qh = (k->qh + 1) % UQ;
    k->qn--;
    net_unlock();
    return (int)n;
}

int udp_readable(int s) { return s >= 0 && s < NUSK && usks[s].used && usks[s].qn > 0; }
uint16_t udp_local_port(int s) { return s >= 0 && s < NUSK ? usks[s].lport : 0; }
void udp_close(int s) { if (s >= 0 && s < NUSK) { net_lock(); usks[s].used = 0; net_unlock(); } }

/* ── receive path ───────────────────────────────────────────────────── */
static void ip_input(const uint8_t *p, size_t len)
{
    if (len < sizeof(struct ip4)) return;
    const struct ip4 *h = (const struct ip4 *)p;
    size_t hl = (size_t)(h->vhl & 15) * 4, tl = ntohs(h->len);
    if ((h->vhl >> 4) != 4 || hl < 20 || tl > len || tl < hl) return;
    if (ntohs(h->frag) & 0x3FFF) return;                       /* no fragment reassembly */
    uint32_t dst = ntohl(h->dst), src = ntohl(h->src);
    if (my_ip && dst != my_ip && dst != 0xFFFFFFFF && (dst | my_mask) != 0xFFFFFFFF) return;
    const uint8_t *pl = p + hl;
    size_t pll = tl - hl;
    switch (h->proto) {
    case 1:  icmp_input(src, pl, pll); break;
    case 6:  tcp_input(src, pl, pll); break;
    case 17: cur_src = src; udp_input(pl, pll); break;
    }
}

static void poll_locked(void)
{
    if (!have_nic) return;
    for (int n = 0; n < 64; n++) {
        int len = nic.recv(&nic, frame_buf, sizeof frame_buf);
        if (len > 0) { net_rx_bytes += (uint64_t)len; net_rx_packets++; }
        if (len <= 0) break;
        if ((size_t)len < sizeof(struct eth)) continue;
        const struct eth *e = (const struct eth *)frame_buf;
        uint16_t type = ntohs(e->type);
        if (type == 0x0806 && (size_t)len >= sizeof *e + sizeof(struct arp)) arp_input((const struct arp *)(frame_buf + sizeof *e));
        else if (type == 0x0800) {
            const struct ip4 *ih = (const struct ip4 *)(frame_buf + sizeof *e);
            if ((size_t)len >= sizeof *e + sizeof *ih && my_ip && ntohl(ih->dst) == my_ip) arp_learn(ntohl(ih->src), e->src);
            ip_input(frame_buf + sizeof *e, (size_t)len - sizeof *e);
        }
    }
    tcp_timers();
}

int net_wait_ready(int timeout_ms)
{
    for (uint32_t t0 = now_ms(); !ready && have_nic && (int32_t)(now_ms() - t0) < timeout_ms; ) wait_tick();
    return ready ? 0 : -1;
}

/* a network adapter that turned up later (USB Ethernet, a phone tethering over USB): it's used
 * if there's no card yet, or if the card has no cable and no address yet */
static struct nic pending;
static volatile int have_pending;
void net_attach(const struct nic *n)
{
    pending = *n;
    have_pending = 1;
}

static int take_pending(void)
{
    if (!have_pending) return 0;
    net_lock();
    nic = pending;
    have_pending = 0;
    have_nic = 1;
    net_unlock();
    return 1;
}

/* ── the net thread ─────────────────────────────────────────────────── */
static void net_thread(uintptr_t arg)
{
    (void)arg;
    for (int i = 0; i < pci_count() && !have_nic; i++) {
        struct pci_dev *d = pci_get(i);
        if (d->class != 0x02 || d->subclass != 0x00) continue;
        memset(&nic, 0, sizeof nic);
        if (e1000_probe(d, &nic) == 0 || igb_probe(d, &nic) == 0 || rtl8169_probe(d, &nic) == 0) have_nic = 1;
    }
    if (!have_nic) {
        const struct pci_dev *eth = pci_find_class(0x02, 0x00, 0), *wifi = pci_wifi();
        char nm[64];
        if (eth) { pci_describe(eth, nm, sizeof nm); ksnprintf(status_line, sizeof status_line, "no driver for %s (USB tethering works)", nm); }
        else if (wifi) { pci_describe(wifi, nm, sizeof nm); ksnprintf(status_line, sizeof status_line, "Wi-Fi (%s) isn't supported: plug in a phone with USB tethering on", nm); }
        else ksnprintf(status_line, sizeof status_line, "no network adapter: plug in a phone with USB tethering on, or a USB Ethernet adapter");
        while (!take_pending()) wait_tick();                 /* until something's plugged in */
    }
    char mac[24];
    ksnprintf(mac, sizeof mac, "%02x:%02x:%02x:%02x:%02x:%02x", nic.mac[0], nic.mac[1], nic.mac[2], nic.mac[3], nic.mac[4], nic.mac[5]);
    ksnprintf(status_line, sizeof status_line, "%s (%s), waiting for link", nic.model, nic.driver);
    for (int tries = 0; ; tries++) {
        if (have_pending && !nic.link(&nic)) take_pending();   /* no cable, but a phone's plugged in */
        if (!nic.link(&nic)) {
            ksnprintf(status_line, sizeof status_line, "%s: cable unplugged", nic.model);
            for (int i = 0; i < 100; i++) wait_tick();
            continue;
        }
        ksnprintf(status_line, sizeof status_line, "%s: asking for an address (DHCP)", nic.model);
        net_lock();
        int r = dhcp_run();
        net_unlock();
        if (r == 0) break;
        ksnprintf(status_line, sizeof status_line, "%s: no DHCP answer, retrying", nic.model);
        for (int i = 0; i < 200; i++) wait_tick();
    }
    char ip[16], gw[16];
    ip_fmt(my_ip, ip, sizeof ip);
    ip_fmt(my_gw, gw, sizeof gw);
    ksnprintf(status_line, sizeof status_line, "%s via %s (%s), gateway %s", ip, nic.driver, nic.model, gw);
    ready = 1;
    tls_seed();
    {   /* for Linux programs: the DNS server DHCP gave us, and the usual name service files */
        char dns[16], rc[64];
        ip_fmt(my_dns ? my_dns : my_gw, dns, sizeof dns);
        int n = ksnprintf(rc, sizeof rc, "nameserver %s\n", dns);
        vfs_write("/etc/resolv.conf", rc, (size_t)n);
        if (!vfs_exists("/etc/hosts")) { static const char h[] = "127.0.0.1 localhost\n::1 localhost\n"; vfs_write("/etc/hosts", h, sizeof h - 1); }
        if (!vfs_exists("/etc/nsswitch.conf")) { static const char ns[] = "passwd: files\ngroup: files\nhosts: files dns\n"; vfs_write("/etc/nsswitch.conf", ns, sizeof ns - 1); }
    }
    for (;;) {
        net_lock();
        poll_locked();
        net_unlock();
        wait_tick();
    }
}

void net_init(void)
{
    rs_spawn("net", net_thread, 0);
}
