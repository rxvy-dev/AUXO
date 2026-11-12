/* Mounts, namespaces, capabilities and seccomp: what a container or a sandbox (bubblewrap,
 * which WebKit and Flatpak use) needs from the kernel. The semantics are Linux's.
 *
 * Mounts: each mount namespace has a list of mounts. A mount shows `root` (a directory or
 * file node, from anywhere: the boot tree, a tmpfs, procfs) on top of the node `mp` as seen
 * through the mount `parent`. A path is a (mount, node) pair, so the same node can be
 * reachable read-write through one mount and read-only through another. */
#ifndef AUXO_NS_H
#define AUXO_NS_H
#include <stdint.h>
#include <stddef.h>

struct node;
struct task;

/* mount flags (Linux's MS_ values for the per-mount ones) */
#define MNT_RDONLY  1
#define MNT_NOSUID  2
#define MNT_NODEV   4
#define MNT_NOEXEC  8
#define MNT_NOATIME 1024
#define MNT_PERMASK (MNT_RDONLY | MNT_NOSUID | MNT_NODEV | MNT_NOEXEC | MNT_NOATIME)

struct mntns;
struct pidns;

struct mount {
    struct mount *parent;           /* 0: the namespace's root mount */
    struct node  *mp;               /* the node it covers, seen through parent */
    struct node  *root;             /* what it shows */
    int           flags;            /* MNT_* */
    int           id;
    char          fstype[12];
    char          source[48];
    struct pidns *procns;           /* procfs: whose processes it shows */
    struct mntns *ns;
    struct mount *next;
};

struct mntns { int refs; uint32_t inum; struct mount *mounts, *root; };

struct path { struct mount *m; struct node *n; };

/* namespaces other than mount: an id (the inode number /proc/PID/ns/... shows) and a count */
#define PID_LEVELS 4
struct pidns  { int refs; uint32_t inum; int level; struct pidns *parent; int next_pid; int init_tgid; int dying; };
struct utsns  { int refs; uint32_t inum; char host[65], domain[65]; int init; };
struct netns  { int refs; uint32_t inum; int isolated; int lo_up; };
struct simplens { int refs; uint32_t inum; };   /* ipc, cgroup: nothing in them to separate yet */

#include "cred.h"

struct seccomp_filter { int refs; struct seccomp_filter *prev; uint32_t len; uint64_t insns[]; };

/* the parts of a task this file deals with (embedded in struct task) */
struct task_ns {
    struct mntns  *mnt;
    struct path    root, cwd, exe;
    struct pidns  *pid;             /* the namespace the task is in */
    struct pidns  *pid_children;    /* where its children go (unshare(CLONE_NEWPID)) */
    int            lpid[PID_LEVELS], ltgid[PID_LEVELS];   /* its ids at each level of nesting */
    struct utsns  *uts;
    struct netns  *net;
    struct simplens *ipc, *cgroup;
    uint64_t       cap_eff, cap_perm, cap_inh, cap_bset, cap_amb;
    int            no_new_privs, dumpable, keepcaps, pdeathsig, subreaper, seccomp_mode;
    struct seccomp_filter *filter;
    struct ucred   cred;            /* user and group ids */
};

#define CAP_ALL      ((1ULL << 41) - 1)    /* CAP_LAST_CAP = 40 (CAP_CHECKPOINT_RESTORE) */
#define CAP_SYS_CHROOT 18
#define CAP_SYS_ADMIN  21

/* mount.c */
void   mount_init(void);                             /* the first namespace, / and /proc */
struct mntns *mntns_init(void);
struct mount *mount_rootm(void);                     /* the first namespace's root mount */
int    vfs_walk(struct task *t, struct path base, const char *path, int follow, struct path *out);
int    vfs_walk_parent(struct task *t, struct path base, const char *path, struct path *dir, char *name, size_t n);
int    vfs_pathname(struct task *t, struct path p, char *buf, size_t len);    /* 0, or 1 if outside the root */
int    vfs_readonly(struct mount *m);
struct mntns *mntns_copy(struct mntns *old, struct task *t);   /* CLONE_NEWNS; moves t's root/cwd over */
void   mntns_put(struct mntns *ns);
long   sys_mount(struct task *t, uint64_t usrc, uint64_t utgt, uint64_t utype, uint64_t flags, uint64_t udata);
long   sys_umount2(struct task *t, uint64_t utgt, long flags);
long   sys_pivot_root(struct task *t, uint64_t unew, uint64_t uold);
long   sys_chroot(struct task *t, uint64_t upath);
int    mount_info(struct task *t, char *buf, size_t cap, int mtab);           /* /proc/self/mountinfo, mounts */
struct mount *mount_of_root(struct mntns *ns, struct node *n, struct mount *parent);

/* procfs.c */
struct node *procfs_new_root(struct pidns *ns);
int    procfs_is(struct node *n);
struct node *procfs_lookup(struct task *t, struct node *dir, const char *name);
int    procfs_list(struct task *t, struct node *dir, uint64_t idx, char *name, size_t n, uint64_t *ino, uint8_t *type);
int    procfs_magic(struct task *t, struct path at, struct path *out);       /* /proc/PID/fd/N, cwd, root, exe, ns links */
int    procfs_readlink(struct task *t, struct node *n, char *buf, size_t len);   /* 1 if n is one of procfs's links */
void   procfs_gc(void);

/* ns.c */
void   ns_task_init(struct task *t, struct task *parent);   /* parent 0: the first namespaces */
void   ns_task_exit(struct task *t);
int    ns_clone(struct task *parent, struct task *child, uint64_t flags);   /* 0 or -errno; before the child runs */
void   ns_child_pid(struct task *parent, struct task *child, int global_pid, int global_tgid);
long   sys_unshare(struct task *t, uint64_t flags);
long   sys_setns(struct task *t, long fd, long nstype);
int    ns_capable(struct task *t, int cap);
long   sys_capget(struct task *t, uint64_t uhdr, uint64_t udata);
long   sys_capset(struct task *t, uint64_t uhdr, uint64_t udata);
long   sys_prctl(struct task *t, long opt, uint64_t a, uint64_t b, uint64_t c, uint64_t d);
void   ns_exec(struct task *t);                     /* capabilities after execve */
int    pid_visible(struct task *viewer, struct task *x);
int    pid_in(struct pidns *ns, struct task *x, int tgid);   /* x's pid (tgid=1: its process id) seen from ns; 0 if not */
int    pid_vpid(struct task *viewer, struct task *x);    /* thread id seen by viewer */
int    pid_vtgid(struct task *viewer, struct task *x);   /* process id seen by viewer */
int    pid_global(struct task *viewer, int vpid, int tgid);   /* global pid (tid or tgid) of what viewer calls vpid; 0 if none */
int    pid_reaper(struct task *t);                  /* global tgid that t's orphans go to, 0: none */
uint32_t ns_inum_next(void);
const char *uts_host(struct task *t);
long   sys_sethostname(struct task *t, uint64_t u, uint64_t len, int domain);
int    net_isolated(struct task *t);

/* seccomp.c */
long   sys_seccomp(struct task *t, long op, long flags, uint64_t uargs);
long   seccomp_set_filter(struct task *t, uint64_t ufprog, long flags);
int    seccomp_check(struct task *t, long nr, const uint64_t args[6], uint64_t ip, long *ret);   /* 1: run it */
void   seccomp_put(struct seccomp_filter *f);

/* netlink.c: NETLINK_ROUTE, enough for `ip link set lo up` in a new network namespace */
long   netlink_socket(struct task *t, long type, long proto);
#endif
