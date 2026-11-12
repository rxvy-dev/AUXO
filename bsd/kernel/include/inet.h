#ifndef AUXO_INET_H
#define AUXO_INET_H
/* AF_INET sockets for Linux programs (kern/inet.c). Addresses in host byte order. */
#include <stdint.h>
#include <stddef.h>
struct task;
enum { INET_STREAM = 1, INET_DGRAM = 2 };
struct isock {
    int type, h, connected, shut_wr;     /* h: TCP or UDP socket in net.c */
    uint32_t dip; uint16_t dport, lport;
};
struct isock *inet_new(int type);
long inet_bind(struct isock *s, uint32_t ip, uint16_t port);
long inet_connect(struct isock *s, uint32_t ip, uint16_t port);
long inet_recv(struct task *t, struct isock *s, void *buf, size_t n, int nonblock, uint32_t *ip, uint16_t *port);
long inet_send(struct isock *s, const void *buf, size_t n, int to, uint32_t ip, uint16_t port);
void inet_shutdown(struct isock *s, int how);
int  inet_poll(struct isock *s);
void inet_names(struct isock *s, uint32_t *lip, uint16_t *lport, uint32_t *rip, uint16_t *rport);
void inet_close(struct isock *s);
#endif
