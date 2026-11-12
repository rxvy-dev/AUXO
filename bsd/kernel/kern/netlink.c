/* NETLINK_ROUTE inside a new network namespace. Such a namespace has one interface, lo,
 * down and without an address until someone (bubblewrap) asks for 127.0.0.1 and up; it
 * answers link and address dumps so programs see exactly that. The first namespace keeps
 * answering EAFNOSUPPORT for netlink, as it always has (glibc falls back without it). */
#include "kern.h"
#include "arch.h"
#include "rs.h"
#include "uproc.h"
#include "ns.h"
#include "netlink.h"

enum { ENODEV = 19, EINVAL = 22, ENOMEM = 12, EAGAIN = 11, EFAULT = 14, EOPNOTSUPP = 95, EPROTONOSUPPORT = 93 };

struct nlsock { uint32_t portid; struct netns *ns; uint32_t qlen; int addr; uint8_t q[8192]; };

#define NLMSG_ERROR 2
#define NLMSG_DONE 3
#define NLM_F_MULTI 2
#define NLM_F_ACK 4
#define NLM_F_DUMP 0x300

long netlink_socket(struct task *t, long type, long proto)
{
    if (proto != 0) return -EPROTONOSUPPORT;
    if ((type & 0xF) != 3 && (type & 0xF) != 2) return -EINVAL;      /* SOCK_RAW / SOCK_DGRAM */
    struct nlsock *s = rs_kmalloc(sizeof *s);
    if (!s) return -ENOMEM;
    memset(s, 0, sizeof *s);
    s->ns = t->ns.net;
    s->ns->refs++;
    struct file *f = file_new(10, 0, 2 | (int)(type & 04000));
    if (!f) { rs_kfree(s); return -ENOMEM; }
    f->priv = s;
    strlcpy(f->path, "socket:[netlink]", sizeof f->path);
    return nl_install(t, f, (type & 02000000) != 0);
}

void nl_close(void *priv) { struct nlsock *s = priv; if (s) { s->ns->refs--; rs_kfree(s); } }
int nl_readable(void *priv) { return ((struct nlsock *)priv)->qlen > 0; }

long nl_bind(struct task *t, void *priv, uint64_t ua, uint64_t len)
{
    struct nlsock *s = priv;
    uint8_t sa[12];
    if (len < 12) return -EINVAL;
    if (copyin(t->mm, sa, ua, 12) < 0) return -EFAULT;
    if (sa[0] != 16) return -EINVAL;
    uint32_t pid; memcpy(&pid, sa + 4, 4);
    s->portid = pid ? pid : (uint32_t)pid_vtgid(t, t);
    return 0;
}

long nl_name(struct task *t, void *priv, uint64_t ua, uint64_t ulen)
{
    struct nlsock *s = priv;
    uint8_t sa[12] = { 16, 0 };
    memcpy(sa + 4, &s->portid, 4);
    uint32_t l = 12;
    if (ua && copyout(t->mm, ua, sa, 12) < 0) return -EFAULT;
    return ulen ? copyout(t->mm, ulen, &l, 4) : 0;
}

static void put(struct nlsock *s, const void *p, size_t n)
{
    if (s->qlen + n > sizeof s->q) return;
    memcpy(s->q + s->qlen, p, n);
    s->qlen += (uint32_t)n;
}

static void hdr(uint8_t *b, uint32_t len, uint16_t type, uint16_t flags, uint32_t seq, uint32_t pid)
{
    memcpy(b, &len, 4); memcpy(b + 4, &type, 2); memcpy(b + 6, &flags, 2); memcpy(b + 8, &seq, 4); memcpy(b + 12, &pid, 4);
}

static void ack(struct nlsock *s, const uint8_t *req, int err)
{
    uint8_t b[36];
    uint32_t seq; memcpy(&seq, req + 8, 4);
    hdr(b, 36, NLMSG_ERROR, 0, seq, s->portid);
    memcpy(b + 16, &err, 4);
    memcpy(b + 20, req, 16);
    put(s, b, 36);
}

static size_t attr(uint8_t *b, uint16_t type, const void *data, uint16_t len)
{
    uint16_t l = (uint16_t)(4 + len);
    memcpy(b, &l, 2); memcpy(b + 2, &type, 2); memcpy(b + 4, data, len);
    size_t al = (size_t)((l + 3) & ~3);
    memset(b + 4 + len, 0, al - l);
    return al;
}

static void done(struct nlsock *s, uint32_t seq)
{
    uint8_t b[20];
    hdr(b, 20, NLMSG_DONE, NLM_F_MULTI, seq, s->portid);
    memset(b + 16, 0, 4);
    put(s, b, 20);
}

static void link_msg(struct nlsock *s, uint32_t seq)
{
    uint8_t b[96];
    memset(b, 0, sizeof b);
    uint16_t arphrd = 772;                                   /* ARPHRD_LOOPBACK */
    int idx = 1;
    uint32_t flags = 0x8 | (s->ns->lo_up ? 0x1 | 0x40 | 0x10000 : 0);   /* LOOPBACK, UP, RUNNING, LOWER_UP */
    memcpy(b + 18, &arphrd, 2); memcpy(b + 20, &idx, 4); memcpy(b + 24, &flags, 4);
    size_t o = 32;
    o += attr(b + o, 3, "lo", 3);                            /* IFLA_IFNAME */
    uint32_t mtu = 65536;
    o += attr(b + o, 4, &mtu, 4);                            /* IFLA_MTU */
    hdr(b, (uint32_t)o, 16, NLM_F_MULTI, seq, s->portid);    /* RTM_NEWLINK */
    put(s, b, o);
}

static void addr_msg(struct nlsock *s, uint32_t seq)
{
    uint8_t b[80];
    memset(b, 0, sizeof b);
    b[16] = 2; b[17] = 8; b[18] = 0x80; b[19] = 254;         /* AF_INET /8 PERMANENT scope host */
    int idx = 1; memcpy(b + 20, &idx, 4);
    size_t o = 24;
    uint8_t ip[4] = { 127, 0, 0, 1 };
    o += attr(b + o, 1, ip, 4);                              /* IFA_ADDRESS */
    o += attr(b + o, 2, ip, 4);                              /* IFA_LOCAL */
    o += attr(b + o, 3, "lo", 3);                            /* IFA_LABEL */
    hdr(b, (uint32_t)o, 20, NLM_F_MULTI, seq, s->portid);    /* RTM_NEWADDR */
    put(s, b, o);
}

long nl_send(struct task *t, void *priv, const uint8_t *buf, size_t n)
{
    struct nlsock *s = priv;
    if (!s->portid) s->portid = (uint32_t)pid_vtgid(t, t);
    size_t o = 0;
    while (o + 16 <= n) {
        const uint8_t *m = buf + o;
        uint32_t len, seq; uint16_t type, flags;
        memcpy(&len, m, 4); memcpy(&type, m + 4, 2); memcpy(&flags, m + 6, 2); memcpy(&seq, m + 8, 4);
        if (len < 16 || o + len > n) break;
        int err = 0, replied = 0;
        switch (type) {
        case 16: case 19: {                                  /* RTM_NEWLINK, RTM_SETLINK */
            int idx; uint32_t fl, ch;
            if (len < 32) { err = -EINVAL; break; }
            memcpy(&idx, m + 20, 4); memcpy(&fl, m + 24, 4); memcpy(&ch, m + 28, 4);
            if (idx != 1) { err = -ENODEV; break; }
            if (ch & 1) s->ns->lo_up = (fl & 1) != 0;
            break;
        }
        case 20: case 21: {                                  /* RTM_NEWADDR, RTM_DELADDR */
            int idx;
            if (len < 24) { err = -EINVAL; break; }
            memcpy(&idx, m + 20, 4);
            if (idx != 1) err = -ENODEV;
            break;
        }
        case 18:                                             /* RTM_GETLINK */
            link_msg(s, seq); if (flags & NLM_F_DUMP) done(s, seq); replied = 1; break;
        case 22:                                             /* RTM_GETADDR */
            if (s->ns->lo_up) addr_msg(s, seq);
            done(s, seq); replied = 1; break;
        case 26:                                             /* RTM_GETROUTE */
            done(s, seq); replied = 1; break;
        default: err = -EOPNOTSUPP;
        }
        if (err || ((flags & NLM_F_ACK) && !replied)) ack(s, m, err);
        o += (len + 3) & ~3U;
    }
    return (long)n;
}

long nl_recv(void *priv, uint8_t *buf, size_t n, int peek)
{
    struct nlsock *s = priv;
    if (!s->qlen) return -EAGAIN;
    size_t k = s->qlen < n ? s->qlen : n;
    memcpy(buf, s->q, k);
    if (!peek) { memmove(s->q, s->q + k, s->qlen - k); s->qlen -= (uint32_t)k; }
    return (long)k;
}
