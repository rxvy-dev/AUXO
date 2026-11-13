#ifndef AUXO_PTY_H
#define AUXO_PTY_H
/* pseudo-terminals: /dev/ptmx and /dev/pts/N (kern/pty.c) */
#include <stdint.h>
#include <stddef.h>
struct task; struct file; struct node;

int          pty_is(struct file *f);
int          pty_open(struct task *t, struct file *f, int flags);
void         pty_close(struct file *f);
long         pty_read(struct task *t, struct file *f, void *buf, size_t n);
long         pty_write(struct task *t, struct file *f, const void *buf, size_t n);
int          pty_ready(struct file *f);                  /* 1 readable, 4 writable, 16 hung up */
long         pty_ioctl(struct task *t, struct file *f, uint64_t req, uint64_t arg);
struct node *pty_ctty_node(struct task *t);              /* /dev/tty: the session's terminal, if a pty */
struct node *pty_peer(struct file *f);                   /* TIOCGPTPEER */
void         pty_setsid(struct task *t);
uint64_t     pty_rdev(struct node *n);

#endif
