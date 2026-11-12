#ifndef AUXO_UPROC_H
#define AUXO_UPROC_H
/* User programs: address spaces (mm.c), processes and the Linux system-call interface
 * (uproc.c, sys.c). AuxoBSD runs unmodified x86_64 Linux ELF binaries. */
#include <stdint.h>
#include <stddef.h>
#include "arch.h"

struct node;

/* ── address spaces ─────────────────────────────────────────────────── */
#define VM_READ   1
#define VM_WRITE  2
#define VM_EXEC   4
#define VM_SHARED 8
#define USER_TOP  0x00007FFFFFFFF000ULL
#define MMAP_TOP  0x00007F0000000000ULL

struct vma {
    uint64_t start, end;           /* page aligned, end exclusive */
    uint32_t prot, shared;
    struct node *file;             /* NULL: anonymous */
    uint64_t off;                  /* file offset of `start` */
    uint64_t flen;                 /* bytes of file data from `off`; the rest reads as zero */
    int counted;                   /* this shared mapping has pinned the file's pages (node->maps) */
};

struct mm {
    uint64_t pml4;                 /* physical = kernel address (identity) */
    struct vma *v;
    int nv, cap;
    uint64_t brk_start, brk;
    int refs;
    volatile int lock_owner, lock_depth;   /* threads share an mm: one changes it at a time */
};

void mm_lock(struct mm *m);
void mm_unlock(struct mm *m);

struct mm *mm_new(void);
struct mm *mm_clone(struct mm *m);                 /* fork: copies every private page */
void       mm_put(struct mm *m);
int        mm_map(struct mm *m, uint64_t start, uint64_t len, uint32_t prot, int shared,
                  struct node *file, uint64_t off, uint64_t flen);   /* replaces what was there */
int        mm_unmap(struct mm *m, uint64_t start, uint64_t len);
int        mm_protect(struct mm *m, uint64_t start, uint64_t len, uint32_t prot);
uint64_t   mm_find_free(struct mm *m, uint64_t len, uint64_t below);
int        mm_fault(struct mm *m, uint64_t addr, int write);          /* 0 = handled */
struct vma *mm_vma_at(struct mm *m, uint64_t addr);

/* copying to and from user memory; they fault pages in as needed. 0 = ok, -14 = EFAULT */
int  copyin(struct mm *m, void *dst, uint64_t usrc, size_t n);
int  copyout(struct mm *m, uint64_t udst, const void *src, size_t n);
long copyin_str(struct mm *m, char *dst, uint64_t usrc, size_t cap);  /* length, or -14 / -36 */

/* ── processes ──────────────────────────────────────────────────────── */
#define MAXFD 256

struct file {
    int    refs;
    int    kind;                   /* F_NODE, F_PIPE_R, F_PIPE_W, F_TTY, F_EVENT, F_SOCK */
    int    flags;                  /* O_ flags (O_APPEND, O_NONBLOCK, access mode) */
    struct node *node;
    uint64_t pos;
    void  *priv;
    char   path[128];              /* for /proc/self/fd and diagnostics */
};

struct files {
    int refs;
    struct file *fd[MAXFD];
    uint8_t cloexec[MAXFD];
};

struct sigact { uint64_t handler, flags, restorer, mask; };

struct task {
    int    used, pid, tgid, ppid, pgid;
    int    state;                  /* T_RUN, T_ZOMBIE */
    int    exit_status;            /* wait status (exit code << 8, or signal) */
    int    vfork_done;             /* set when a CLONE_VFORK child execs or exits */
    int    waiting_vfork;
    struct mm *mm;
    struct files *files;
    struct node *cwd;
    uint64_t clear_child_tid;
    uint64_t sigmask, pending;
    uint64_t alarm_at, alarm_every;  /* ITIMER_REAL / alarm(): kernel ticks, 0 = off */
    struct sigact *sig;            /* 64 actions, shared by threads */
    char   name[16];
    char   exe[128];
    uint32_t umask;
    int    is_thread, detached;
    uint64_t start_tick;
    struct trapframe *tf;          /* the trap frame of the current syscall */
};

struct task *task_current(void);   /* NULL on kernel threads */
struct task *task_by_pid(int pid);
int  uproc_spawn(const char *path, char *const argv[], char *const envp[], const char *cwd);   /* pid or -errno */
int  uproc_spawn_detached(const char *path, char *const argv[], char *const envp[], const char *cwd);   /* reaped on exit */
int  uproc_wait(int pid, int *status);                                     /* from kernel threads */
void uproc_kill_fg(int sig);                                               /* Ctrl-C */
int  uproc_count(void);

/* exec.c */
int  exec_load(struct task *t, const char *path, char *const kargv[], char *const kenvp[], struct trapframe *tf);

/* sys.c */
void syscall_dispatch(struct trapframe *tf);
void user_exception(struct trapframe *tf);
void user_return(struct trapframe *tf) __attribute__((noreturn));

/* file layer (file.c) */
struct file *file_new(int kind, struct node *n, int flags);
void  file_put(struct file *f);
long  file_read(struct task *t, struct file *f, void *kbuf, size_t n);
long  file_write(struct task *t, struct file *f, const void *kbuf, size_t n);
int   pipe_new(struct file **r, struct file **w);
int   tty_read(void *kbuf, size_t n, int nonblock);
int   tty_readable(void);

#endif
