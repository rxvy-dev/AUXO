#ifndef AUXO_UNIX_H
#define AUXO_UNIX_H
/* AF_UNIX stream sockets (kern/unix.c) */
#include <stdint.h>
#include <stddef.h>

struct file;
struct node;
#define UNIX_MAXFDS 64

struct usock {
    int refs, listening, connected, peer_closed;
    struct usock *peer;
    uint8_t *rx; uint32_t head, len, cap;
    uint64_t total_in, total_out;                /* byte counters, to place passed fds */
    struct { struct file *f; uint64_t at; } fds[UNIX_MAXFDS];
    int nfds;
    struct usock *backlog[16]; int nback;
    struct node *node;
    int kernel;                                  /* the Wayland server's end */
    int type;                                    /* SOCK_STREAM 1, SOCK_DGRAM 2, SOCK_SEQPACKET 5 */
    /* SOCK_SEQPACKET / SOCK_DGRAM: what was sent arrives as the same messages (WebKit's IPC needs this) */
    uint32_t mlen[512]; int mhead, mcount;
    uint64_t msgs_in, msgs_out;                  /* message counters, to place passed fds */
    int cpid; uint32_t cuid, cgid;               /* who made it (connect / listen): SO_PEERCRED for the other end */
};
#define UNIX_PACKETS(s) ((s)->type == 2 || (s)->type == 5)

struct usock *unix_new(void);
void  unix_close(struct usock *s);
int   unix_bind(struct usock *s, const char *path, struct node *cwd);
int   unix_bind_abstract(struct usock *s, const char *name, uint32_t ns);
int   unix_bind_at(struct usock *s, struct node *dir, const char *name);
int   unix_connect_abstract(struct usock *s, const char *name, uint32_t ns);
int   unix_connect_node(struct usock *s, struct node *n);
int   unix_listen(struct usock *s);
int   unix_connect(struct usock *s, const char *path, struct node *cwd);
struct usock *unix_accept(struct usock *l);
int   unix_pair(struct usock **a, struct usock **b);
long  unix_send(struct usock *s, const void *data, size_t len, struct file **files, int nfiles);
long  unix_recv(struct usock *s, void *buf, size_t len, struct file **files, int *nfiles, int maxfiles);
int   unix_readable(struct usock *s);
int   unix_writable(struct usock *s);

#endif
