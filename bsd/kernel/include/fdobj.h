#ifndef AUXO_FDOBJ_H
#define AUXO_FDOBJ_H
#include <stdint.h>
#include <stddef.h>
struct task; struct file; struct node;
/* kern/fdobj.c: inotify (file kind 11), timerfd (12), pidfd (13), POSIX timers */
void inotify_event(struct node *dir, struct node *n, const char *name, uint32_t mask, uint32_t cookie);   /* fs_lock held */
uint32_t inotify_cookie(void);
long sys_inotify_init(struct task *t, long flags);
long sys_inotify_add(struct task *t, long fd, uint64_t upath, uint32_t mask);
long sys_inotify_rm(struct task *t, long fd, long wd);
long sys_timerfd_create(struct task *t, long clock, long flags);
long sys_timerfd_settime(struct task *t, long fd, long flags, uint64_t unew, uint64_t uold);
long sys_timerfd_gettime(struct task *t, long fd, uint64_t ucur);
long sys_pidfd_open(struct task *t, long pid, long flags);
int  pidfd_pid(struct file *f);
long fdobj_read(struct task *t, struct file *f, void *kbuf, size_t n);
int  fdobj_ready(struct task *t, struct file *f);
void fdobj_close(struct file *f);
long sys_timer_create(struct task *t, long clock, uint64_t usev, uint64_t uid);
long sys_timer_settime(struct task *t, long id, long flags, uint64_t unew, uint64_t uold);
long sys_timer_gettime(struct task *t, long id, uint64_t ucur);
long sys_timer_getoverrun(struct task *t, long id);
long sys_timer_delete(struct task *t, long id);
void ptimer_exit(int tgid);
void ptimer_tick(void);
void clock_now(int clk, int64_t *s, int64_t *ns);          /* sys.c: clock_gettime's clocks */
/* inotify masks fs.c reports */
#define IN_MODIFY 0x2u
#define IN_ATTRIB 0x4u
#define IN_CLOSE_WRITE 0x8u
#define IN_CLOSE_NOWRITE 0x10u
#define IN_OPEN 0x20u
#define IN_MOVED_FROM 0x40u
#define IN_MOVED_TO 0x80u
#define IN_CREATE 0x100u
#define IN_DELETE 0x200u
#define IN_DELETE_SELF 0x400u
#define IN_MOVE_SELF 0x800u
#endif
