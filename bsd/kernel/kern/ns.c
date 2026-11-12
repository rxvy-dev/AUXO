/* Namespaces other than mounts (PID, UTS, network, IPC, cgroup), capabilities and prctl.
 *
 * PID namespaces nest up to PID_LEVELS deep. A task has an id at every level from the first
 * namespace down to its own; a task sees only the tasks whose namespace is its own or below
 * it, by their ids at its level. The first task of a namespace is its init: orphans there
 * go to it, it ignores signals from inside that it has no handler for, and when it exits
 * everything else in the namespace is killed.
 *
 * Everything on AuxoBSD runs as root, so capabilities are what keeps a sandboxed process from
 * remounting or leaving its namespaces once a sandbox drops them; seccomp (seccomp.c) is
 * the other half. User namespaces aren't there: CLONE_NEWUSER fails with EINVAL, which is
 * what programs check for. */
#include "kern.h"
#include "arch.h"
#include "rs.h"
#include "fs.h"
#include "uproc.h"
#include "auxo.h"
#include "ns.h"

enum { EPERM = 1, ESRCH = 3, ENOMEM = 12, EFAULT = 14, EINVAL = 22, ENOSPC = 28, ENOSYS = 38 };

#define CLONE_NEWNS     0x00020000
#define CLONE_NEWCGROUP 0x02000000
#define CLONE_NEWUTS    0x04000000
#define CLONE_NEWIPC    0x08000000
#define CLONE_NEWUSER   0x10000000
#define CLONE_NEWPID    0x20000000
#define CLONE_NEWNET    0x40000000
#define CLONE_THREAD    0x00010000
#define CLONE_FS        0x00000200
#define CLONE_NS_ALL (CLONE_NEWNS | CLONE_NEWCGROUP | CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWPID | CLONE_NEWNET)

static uint32_t inum_next = 0xEFFFFFFB;            /* Linux's first namespaces are 0xEFFFFFFB.. (4026531835..) */
uint32_t ns_inum_next(void) { return inum_next++; }

static struct pidns init_pid;
static struct utsns init_uts;
static struct netns init_net;
static struct simplens init_ipc, init_cgroup;

static void ns_init_once(void)
{
    if (init_pid.inum) return;
    init_cgroup.inum = ns_inum_next(); init_cgroup.refs = 1;
    init_ipc.inum = ns_inum_next(); init_ipc.refs = 1;
    init_uts.inum = ns_inum_next(); init_uts.refs = 1; init_uts.init = 1;
    strlcpy(init_uts.domain, "(none)", sizeof init_uts.domain);
    init_pid.inum = ns_inum_next(); init_pid.refs = 1; init_pid.level = 0;
    init_net.inum = ns_inum_next(); init_net.refs = 1;
}

/* ── tasks ──────────────────────────────────────────────────────────── */
void ns_task_init(struct task *t, struct task *p)
{
    ns_init_once();
    struct task_ns *n = &t->ns;
    if (!p) {
        memset(n, 0, sizeof *n);
        n->mnt = mntns_init(); n->mnt->refs++;
        n->root.m = n->cwd.m = mount_rootm(); n->root.n = n->cwd.n = fs_root();
        n->pid = &init_pid; init_pid.refs++;
        n->uts = &init_uts; init_uts.refs++;
        n->net = &init_net; init_net.refs++;
        n->ipc = &init_ipc; init_ipc.refs++;
        n->cgroup = &init_cgroup; init_cgroup.refs++;
        n->cap_eff = n->cap_perm = n->cap_bset = CAP_ALL;
        n->dumpable = 1;
        return;
    }
    *n = p->ns;
    n->mnt->refs++;
    n->pid = p->ns.pid_children ? p->ns.pid_children : p->ns.pid;
    n->pid->refs++;
    n->pid_children = 0;
    n->uts->refs++; n->net->refs++; n->ipc->refs++; n->cgroup->refs++;
    if (n->filter) n->filter->refs++;
    n->pdeathsig = 0;
    n->subreaper = 0;
    memset(n->lpid, 0, sizeof n->lpid);
    memset(n->ltgid, 0, sizeof n->ltgid);
}

void ns_task_exit(struct task *t)
{
    struct task_ns *n = &t->ns;
    if (!n->pid) return;
    /* the parent of this process asked to hear of its death */
    if (!t->is_thread)
        for (int i = 0; i < MAXTASK; i++) {
            struct task *c = task_slot(i);
            if (c->used && c != t && c->ppid == t->tgid && c->ns.pdeathsig > 0 && c->ns.pdeathsig <= 64)
                c->pending |= 1ULL << (c->ns.pdeathsig - 1);
        }
    /* a namespace's init: everything left in it goes */
    if (!t->is_thread && n->pid->level > 0 && n->pid->init_tgid == t->tgid) {
        struct pidns *dead = n->pid;
        dead->dying = 1;
        for (int i = 0; i < MAXTASK; i++) {
            struct task *o = task_slot(i);
            if (!o->used || o == t || o->state == 2 || !o->ns.pid) continue;
            for (struct pidns *q = o->ns.pid; q; q = q->parent) if (q == dead) { o->pending |= 1ULL << 8; break; }   /* SIGKILL */
        }
    }
    if (n->filter) { seccomp_put(n->filter); n->filter = 0; }
    mntns_put(n->mnt);
}

/* where orphans go: the nearest living ancestor that asked to be a subreaper, else the
 * namespace's init; 0 in the first namespace (as before: nobody) */
int pid_reaper(struct task *t)
{
    struct task *a = t->ppid > 0 ? task_by_pid(t->ppid) : 0;
    for (int guard = 0; a && guard < MAXTASK; guard++) {
        if (a->state != 2 && a->ns.subreaper && a->ns.pid == t->ns.pid) return a->tgid;
        a = a->ppid > 0 ? task_by_pid(a->ppid) : 0;
    }
    struct pidns *ns = t->ns.pid;
    if (ns && ns->level > 0 && !ns->dying && ns->init_tgid && ns->init_tgid != t->tgid) {
        struct task *i = task_by_pid(ns->init_tgid);
        if (i && i->used && i->state != 2) return ns->init_tgid;
    }
    return 0;
}

/* ── ids ────────────────────────────────────────────────────────────── */
static struct pidns *at_level(struct pidns *ns, int level)
{
    while (ns && ns->level > level) ns = ns->parent;
    return ns && ns->level == level ? ns : 0;
}

int pid_in(struct pidns *ns, struct task *x, int tgid)
{
    if (!x->ns.pid || !ns || ns->level == 0) return tgid ? x->tgid : x->pid;
    if (at_level(x->ns.pid, ns->level) != ns) return 0;
    return tgid ? x->ns.ltgid[ns->level] : x->ns.lpid[ns->level];
}

int pid_visible(struct task *v, struct task *x) { return pid_in(v->ns.pid, x, 0) != 0; }
int pid_vpid(struct task *v, struct task *x) { return pid_in(v->ns.pid, x, 0); }
int pid_vtgid(struct task *v, struct task *x) { return pid_in(v->ns.pid, x, 1); }

int pid_global(struct task *v, int vpid, int tgid)
{
    if (vpid <= 0) return 0;
    if (!v->ns.pid || v->ns.pid->level == 0) return vpid;
    for (int i = 0; i < MAXTASK; i++) {
        struct task *o = task_slot(i);
        if (!o->used || (tgid && o->is_thread)) continue;
        if (pid_in(v->ns.pid, o, tgid) == vpid) return tgid ? o->tgid : o->pid;
    }
    return 0;
}

/* ids at levels 1.. for a new task (level 0 is the scheduler's pid, filled in when known) */
void ns_child_pid(struct task *p, struct task *c, int gpid, int gtgid)
{
    struct pidns *ns = c->ns.pid;
    int thread = c->is_thread;
    c->ns.lpid[0] = gpid; c->ns.ltgid[0] = gtgid;
    for (int l = 1; ns && l <= ns->level; l++) {
        struct pidns *at = at_level(ns, l);
        int id = at->next_pid++ + 1;
        if (id == 1 && !thread) at->init_tgid = -1;          /* known once the global pid is */
        c->ns.lpid[l] = id;
        c->ns.ltgid[l] = thread ? p->ns.ltgid[l] : id;
    }
}

/* ── clone / unshare ────────────────────────────────────────────────── */
static void *zalloc(size_t n) { void *p = rs_kmalloc(n); if (p) memset(p, 0, n); return p; }

static int new_ns(struct task_ns *n, uint64_t flags, struct task *self)
{
    if (flags & CLONE_NEWUTS) {
        struct utsns *u = zalloc(sizeof *u);
        if (!u) return -ENOMEM;
        strlcpy(u->host, uts_host(self), sizeof u->host);
        strlcpy(u->domain, n->uts->domain, sizeof u->domain);
        u->refs = 1; u->inum = ns_inum_next();
        n->uts->refs--; n->uts = u;
    }
    if (flags & CLONE_NEWIPC) {
        struct simplens *s = zalloc(sizeof *s);
        if (!s) return -ENOMEM;
        s->refs = 1; s->inum = ns_inum_next();
        n->ipc->refs--; n->ipc = s;
    }
    if (flags & CLONE_NEWCGROUP) {
        struct simplens *s = zalloc(sizeof *s);
        if (!s) return -ENOMEM;
        s->refs = 1; s->inum = ns_inum_next();
        n->cgroup->refs--; n->cgroup = s;
    }
    if (flags & CLONE_NEWNET) {
        struct netns *s = zalloc(sizeof *s);
        if (!s) return -ENOMEM;
        s->refs = 1; s->inum = ns_inum_next(); s->isolated = 1;
        n->net->refs--; n->net = s;
    }
    if (flags & CLONE_NEWNS) {
        struct mntns *m = mntns_copy(n->mnt, self);
        if (!m) return -ENOMEM;
        mntns_put(n->mnt);
        n->mnt = m;
    }
    return 0;
}

static struct pidns *pidns_new(struct pidns *parent)
{
    if (parent->level + 1 >= PID_LEVELS) return 0;
    struct pidns *p = zalloc(sizeof *p);
    if (!p) return 0;
    p->refs = 1; p->inum = ns_inum_next(); p->level = parent->level + 1; p->parent = parent;
    return p;
}

int ns_clone(struct task *p, struct task *c, uint64_t flags)
{
    if (flags & CLONE_NEWUSER) return -EINVAL;
    if (!(flags & CLONE_NS_ALL)) return 0;
    if (!ns_capable(p, CAP_SYS_ADMIN)) return -EPERM;
    if ((flags & CLONE_THREAD) && (flags & (CLONE_NEWPID | CLONE_NEWNS))) return -EINVAL;
    if (flags & CLONE_NEWPID) {
        struct pidns *n = pidns_new(c->ns.pid);
        if (!n) return -ENOSPC;
        c->ns.pid->refs--;
        c->ns.pid = n;
    }
    return new_ns(&c->ns, flags, c);
}

long sys_unshare(struct task *t, uint64_t flags)
{
    if (flags & CLONE_NEWUSER) return -EINVAL;
    if (flags & ~(CLONE_NS_ALL | CLONE_FS | 0x400 | 0x8000000 | 0x80000000UL)) return -EINVAL;
    if (!(flags & CLONE_NS_ALL)) return 0;
    if (!ns_capable(t, CAP_SYS_ADMIN)) return -EPERM;
    if (flags & CLONE_NEWPID) {                               /* for the children from now on */
        if (t->ns.pid_children) return -EINVAL;
        struct pidns *n = pidns_new(t->ns.pid);
        if (!n) return -ENOSPC;
        t->ns.pid_children = n;
    }
    return new_ns(&t->ns, flags, t);
}

long sys_setns(struct task *t, long fd, long nstype)
{
    (void)t; (void)fd; (void)nstype;
    return -EINVAL;                                           /* joining a namespace by fd: not yet */
}

/* ── UTS and network ────────────────────────────────────────────────── */
const char *uts_host(struct task *t)
{
    if (!t || !t->ns.uts || t->ns.uts->init) return auxo.hostname;
    return t->ns.uts->host;
}

long sys_sethostname(struct task *t, uint64_t u, uint64_t len, int domain)
{
    if (!ns_capable(t, CAP_SYS_ADMIN)) return -EPERM;
    if (len > 64) return -EINVAL;
    char b[65];
    if (len && copyin(t->mm, b, u, len) < 0) return -EFAULT;
    b[len] = 0;
    struct utsns *n = t->ns.uts;
    if (domain) { strlcpy(n->domain, b, sizeof n->domain); return 0; }
    if (n->init) strlcpy(auxo.hostname, b, sizeof auxo.hostname);
    else strlcpy(n->host, b, sizeof n->host);
    return 0;
}

int net_isolated(struct task *t) { return t && t->ns.net && t->ns.net->isolated; }

/* ── capabilities ───────────────────────────────────────────────────── */
int ns_capable(struct task *t, int cap) { return !t || (t->ns.cap_eff >> cap) & 1; }

long sys_capget(struct task *t, uint64_t uhdr, uint64_t udata)
{
    uint32_t h[2];
    if (copyin(t->mm, h, uhdr, 8) < 0) return -EFAULT;
    int words = h[0] == 0x19980330 ? 1 : (h[0] == 0x20071026 || h[0] == 0x20080522) ? 2 : 0;
    if (!words) { h[0] = 0x20080522; copyout(t->mm, uhdr, h, 8); return udata ? -EINVAL : 0; }
    struct task *x = t;
    if ((int)h[1] != 0) { int g = pid_global(t, (int)h[1], 0); x = g ? task_by_pid(g) : 0; if (!x) return -ESRCH; }
    if (!udata) return 0;
    uint32_t d[6];
    for (int w = 0; w < 2; w++) {
        d[w * 3 + 0] = (uint32_t)(x->ns.cap_eff >> (32 * w));
        d[w * 3 + 1] = (uint32_t)(x->ns.cap_perm >> (32 * w));
        d[w * 3 + 2] = (uint32_t)(x->ns.cap_inh >> (32 * w));
    }
    return copyout(t->mm, udata, d, (size_t)words * 12);
}

long sys_capset(struct task *t, uint64_t uhdr, uint64_t udata)
{
    uint32_t h[2];
    if (copyin(t->mm, h, uhdr, 8) < 0) return -EFAULT;
    int words = h[0] == 0x19980330 ? 1 : (h[0] == 0x20071026 || h[0] == 0x20080522) ? 2 : 0;
    if (!words) { h[0] = 0x20080522; copyout(t->mm, uhdr, h, 8); return -EINVAL; }
    if ((int)h[1] != 0 && pid_global(t, (int)h[1], 0) != t->pid) return -EPERM;
    uint32_t d[6] = { 0 };
    if (copyin(t->mm, d, udata, (size_t)words * 12) < 0) return -EFAULT;
    uint64_t eff = d[0] | (uint64_t)d[3] << 32, perm = d[1] | (uint64_t)d[4] << 32, inh = d[2] | (uint64_t)d[5] << 32;
    if (words == 1) { eff |= t->ns.cap_eff & ~0xFFFFFFFFULL; perm |= t->ns.cap_perm & ~0xFFFFFFFFULL; inh |= t->ns.cap_inh & ~0xFFFFFFFFULL; }
    eff &= CAP_ALL; perm &= CAP_ALL; inh &= CAP_ALL;
    uint64_t setpcap = (t->ns.cap_eff >> 8) & 1;
    if (perm & ~t->ns.cap_perm) return -EPERM;                       /* permitted only shrinks */
    if (eff & ~perm) return -EPERM;
    if (inh & ~(t->ns.cap_inh | (setpcap ? t->ns.cap_bset : t->ns.cap_perm))) return -EPERM;
    t->ns.cap_eff = eff; t->ns.cap_perm = perm; t->ns.cap_inh = inh;
    t->ns.cap_amb &= perm & inh;
    return 0;
}

/* after execve: everything runs as root, so a program starts with the bounding set */
void ns_exec(struct task *t)
{
    t->ns.cap_perm = (t->ns.cap_bset | (t->ns.cap_inh & t->ns.cap_bset)) & CAP_ALL;
    t->ns.cap_eff = t->ns.cap_perm;
    t->ns.cap_amb &= t->ns.cap_perm;
    t->ns.keepcaps = 0;
    t->ns.dumpable = 1;
}

/* ── prctl ──────────────────────────────────────────────────────────── */
long sys_prctl(struct task *t, long opt, uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{
    switch (opt) {
    case 1: if (a > 64) return -EINVAL; t->ns.pdeathsig = (int)a; return 0;            /* PR_SET_PDEATHSIG */
    case 2: { int v = t->ns.pdeathsig; return copyout(t->mm, a, &v, 4); }               /* PR_GET_PDEATHSIG */
    case 3: return t->ns.dumpable;                                                     /* PR_GET_DUMPABLE */
    case 4: if (a > 1) return -EINVAL; t->ns.dumpable = (int)a; return 0;
    case 7: return t->ns.keepcaps;
    case 8: if (a > 1) return -EINVAL; t->ns.keepcaps = (int)a; return 0;
    case 15: { char nm[16]; if (copyin_str(t->mm, nm, a, sizeof nm) < 0) { if (copyin(t->mm, nm, a, 15) < 0) return -EFAULT; nm[15] = 0; }
               strlcpy(t->name, nm, sizeof t->name); return 0; }                     /* PR_SET_NAME */
    case 16: { char nm[16]; memset(nm, 0, sizeof nm); strlcpy(nm, t->name, sizeof nm); return copyout(t->mm, a, nm, 16); }
    case 21: return t->ns.seccomp_mode;                                                /* PR_GET_SECCOMP */
    case 22:                                                                           /* PR_SET_SECCOMP */
        if (a == 1) return sys_seccomp(t, 0, 0, 0);
        if (a == 2) return seccomp_set_filter(t, b, 0);
        return -EINVAL;
    case 23: if (a > 40) return -EINVAL; return (int)((t->ns.cap_bset >> a) & 1);      /* PR_CAPBSET_READ */
    case 24:                                                                           /* PR_CAPBSET_DROP */
        if (a > 40) return -EINVAL;
        if (!ns_capable(t, 8)) return -EPERM;                                          /* CAP_SETPCAP */
        t->ns.cap_bset &= ~(1ULL << a);
        return 0;
    case 27: return 0;                                                                 /* PR_GET_SECUREBITS */
    case 28: return ns_capable(t, 8) ? 0 : -EPERM;                                     /* PR_SET_SECUREBITS */
    case 36: t->ns.subreaper = a != 0; return 0;                                       /* PR_SET_CHILD_SUBREAPER */
    case 37: { int v = t->ns.subreaper; return copyout(t->mm, a, &v, 4); }
    case 38: if (a != 1 || b || c || d) return -EINVAL; t->ns.no_new_privs = 1; return 0;   /* PR_SET_NO_NEW_PRIVS */
    case 39: if (a || b || c || d) return -EINVAL; return t->ns.no_new_privs;
    case 47:                                                                           /* PR_CAP_AMBIENT */
        if (a == 4) { t->ns.cap_amb = 0; return 0; }
        if (b > 40) return -EINVAL;
        if (a == 1) return (int)((t->ns.cap_amb >> b) & 1);
        if (a == 2) {
            if (!((t->ns.cap_perm >> b) & 1) || !((t->ns.cap_inh >> b) & 1)) return -EPERM;
            t->ns.cap_amb |= 1ULL << b; return 0;
        }
        if (a == 3) { t->ns.cap_amb &= ~(1ULL << b); return 0; }
        return -EINVAL;
    case 0x53564D41: return -EINVAL;                                                   /* PR_SET_VMA: optional */
    }
    return 0;                                                                          /* timer slack, THP, ptracer …: accepted */
}
