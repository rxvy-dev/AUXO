#ifndef AUXO_NETLINK_H
#define AUXO_NETLINK_H
#include <stdint.h>
#include <stddef.h>
struct task; struct file;
/* netlink sockets are files of kind 10 (see netlink.c) */
int  nl_install(struct task *t, struct file *f, int cloexec);   /* in sys.c: put f in a free fd */
void nl_close(void *priv);
int  nl_readable(void *priv);
long nl_bind(struct task *t, void *priv, uint64_t ua, uint64_t len);
long nl_name(struct task *t, void *priv, uint64_t ua, uint64_t ulen);
long nl_send(struct task *t, void *priv, const uint8_t *buf, size_t n);
long nl_recv(void *priv, uint8_t *buf, size_t n, int peek);
#endif
