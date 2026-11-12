/* AF_INET sockets for Linux programs, on the kernel's TCP/IP stack (net/net.c): TCP clients and
 * UDP. That's what curl, wget, Python, pip, git and the package manager need. No listening TCP
 * sockets (yet) and no IPv6: socket(AF_INET6) fails, and programs fall back to IPv4. */
#include "kern.h"
#include "arch.h"
#include "rs.h"
#include "uproc.h"
#include "net.h"
#include "inet.h"

enum { EINTR_ = 4, EAGAIN_ = 11, EINVAL_ = 22, EPIPE_ = 32, EDESTADDRREQ_ = 89, EMSGSIZE__ = 90, EADDRINUSE_ = 98,
       ENETUNREACH_ = 101, ECONNRESET_ = 104, EISCONN_ = 106, ENOTCONN_ = 107, ECONNREFUSED_ = 111 };

static int interrupted(struct task *t) { return t && (t->pending & ~t->sigmask); }

struct isock *inet_new(int type)
{
    struct isock *s = rs_kmalloc(sizeof *s);
    if (!s) return 0;
    memset(s, 0, sizeof *s);
    s->type = type;
    s->h = -1;
    if (type == INET_DGRAM && (s->h = udp_open()) < 0) { rs_kfree(s); return 0; }
    return s;
}

long inet_bind(struct isock *s, uint32_t ip, uint16_t port)
{
    (void)ip;
    if (s->type == INET_DGRAM) return udp_bind(s->h, port) == 0 ? 0 : -EADDRINUSE_;
    s->lport = port;                         /* TCP: remembered only (we're a client) */
    return 0;
}

long inet_connect(struct isock *s, uint32_t ip, uint16_t port)
{
    if (s->type == INET_DGRAM) {
        if (udp_connect(s->h, ip, port) < 0) return -EINVAL_;
        s->dip = ip; s->dport = port; s->connected = ip != 0;
        return 0;
    }
    if (s->connected) return -EISCONN_;
    if ((ip >> 24) == 127 || !ip) return -ECONNREFUSED_;        /* no loopback interface */
    if (!net_ready() && net_wait_ready(8000) < 0) return -ENETUNREACH_;
    int h = tcp_connect(ip, port, 20000);
    if (h < 0) return -ECONNREFUSED_;
    s->h = h;
    s->dip = ip; s->dport = port;
    s->connected = 1;
    return 0;
}

long inet_recv(struct task *t, struct isock *s, void *buf, size_t n, int nonblock, uint32_t *ip, uint16_t *port)
{
    if (s->type == INET_DGRAM) {
        for (;;) {
            int r = udp_recvfrom(s->h, buf, n, ip, port);
            if (r >= 0) return r;
            if (r != -2) return -EINVAL_;
            if (nonblock) return -EAGAIN_;
            if (interrupted(t)) return -EINTR_;
            wait_tick();
        }
    }
    if (!s->connected) return -ENOTCONN_;
    if (ip) *ip = s->dip;
    if (port) *port = s->dport;
    for (;;) {
        int r = tcp_recv(s->h, buf, n, nonblock ? 0 : 50);
        if (r >= 0) return r;
        if (r == -1) return -ECONNRESET_;
        if (nonblock) return -EAGAIN_;
        if (interrupted(t)) return -EINTR_;
    }
}

long inet_send(struct isock *s, const void *buf, size_t n, int to, uint32_t ip, uint16_t port)
{
    if (s->type == INET_DGRAM) {
        if (!to) {
            if (!s->connected) return -EDESTADDRREQ_;
            ip = s->dip; port = s->dport;
        }
        int r = udp_sendto(s->h, ip, port, buf, n);
        return r >= 0 ? r : r == -2 ? -EMSGSIZE__ : -ENETUNREACH_;
    }
    if (!s->connected) return -ENOTCONN_;
    if (s->shut_wr) return -EPIPE_;
    int r = tcp_send(s->h, buf, n);
    return r >= 0 ? r : -EPIPE_;
}

void inet_shutdown(struct isock *s, int how)
{
    if (s->type == INET_STREAM && s->connected && how >= 1 && !s->shut_wr) { tcp_shutdown(s->h); s->shut_wr = 1; }
}

int inet_poll(struct isock *s)                  /* POLLIN 1, POLLOUT 4, POLLHUP 0x10 */
{
    if (s->type == INET_DGRAM) return (udp_readable(s->h) ? 1 : 0) | 4;
    if (!s->connected) return 4;
    int r = 0;
    if (tcp_readable(s->h)) r |= 1;
    if (tcp_writable(s->h)) r |= 4;
    if (tcp_hup(s->h)) r |= 0x10;
    return r;
}

void inet_names(struct isock *s, uint32_t *lip, uint16_t *lport, uint32_t *rip, uint16_t *rport)
{
    *lip = net_ip(); *lport = s->lport; *rip = s->dip; *rport = s->dport;
    if (s->type == INET_DGRAM) *lport = udp_local_port(s->h);
    else if (s->connected) { uint32_t a, b; uint16_t c; tcp_endpoints(s->h, &a, lport, &b, &c); }
}

void inet_close(struct isock *s)
{
    if (s->type == INET_DGRAM) udp_close(s->h);
    else if (s->connected) tcp_close(s->h);
    rs_kfree(s);
}
