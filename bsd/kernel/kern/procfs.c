/* procfs: /proc as a filesystem of its own, mounted at /proc and mountable again inside a
 * namespace (a sandbox's /proc shows only the processes of its PID namespace).
 *
 * Nothing is stored: a lookup makes a node on the spot, a file's text is made when it's
 * looked up, a directory is listed from the processes and files that exist right then.
 * Made nodes live in a list and are freed once nothing uses them and they are a few
 * seconds old. Links such as /proc/self/fd/3 are "magic": following one goes to the open
 * file itself (its mount and node), not to the text readlink shows. */
#include "kern.h"
#include "arch.h"
#include "rs.h"
#include "fs.h"
#include "uproc.h"
#include "auxo.h"
#include "ns.h"

#define PROC_DEV 0x50524F43u              /* "PROC" in node->dev: a procfs directory or link */

enum { P_ROOT = 1, P_DIR, P_PID, P_FDDIR, P_NSDIR, P_TASKDIR, P_FILE, P_LINK, P_FD, P_CWD, P_ROOTLNK, P_EXE, P_NS, P_NSNODE };

struct pent {
    int kind, pid, fd;                    /* pid: global tgid (P_PID and below), fd: P_FD / P_NS index */
    struct pidns *ns;                     /* the mount's PID namespace */
    char path[48];                        /* P_DIR: where in /proc ("sys/kernel") */
    uint64_t born;
    struct node *node, *gcnext;
    int permanent;
};

static struct node *made;                 /* every made node, for procfs_gc */
static uint32_t next_ino = 0x10000000;

static struct pent *pe(struct node *n) { return n && n->dev == PROC_DEV ? n->priv : 0; }
int procfs_is(struct node *n) { struct pent *p = pe(n); return p && n->type == N_DIR; }

/* nodes made here keep their directory alive (n->parent) while they live */
static struct node *mk(struct node *dir, const char *name, int type, uint16_t mode, int kind, int pid, int fd, const char *path)
{
    struct node *n = rs_kmalloc(sizeof *n);
    struct pent *p = rs_kmalloc(sizeof *p);
    if (!n || !p) { if (n) rs_kfree(n); if (p) rs_kfree(p); return 0; }
    memset(n, 0, sizeof *n);
    memset(p, 0, sizeof *p);
    size_t l = strlen(name);
    n->name = rs_kmalloc(l + 1);
    if (!n->name) { rs_kfree(n); rs_kfree(p); return 0; }
    memcpy(n->name, name, l + 1);
    n->type = (uint8_t)type; n->mode = mode; n->nlink = 1;
    n->ino = next_ino++;
    n->dev = PROC_DEV; n->priv = p;
    n->mtime = fs_now();
    n->parent = dir ? dir : n;
    if (dir) dir->refs++;
    if (pid > 0) { struct task *x = task_by_pid(pid); if (x) { n->uid = x->ns.cred.euid; n->gid = x->ns.cred.egid; } }
    p->kind = kind; p->pid = pid; p->fd = fd; p->node = n;
    p->ns = dir && pe(dir) ? pe(dir)->ns : 0;
    if (path) strlcpy(p->path, path, sizeof p->path);
    p->born = kernel_ticks();
    p->gcnext = made; made = n;
    return n;
}

static void set_text(struct node *n, const char *s, size_t len)
{
    uint8_t *d = len ? rs_kmalloc(len) : 0;
    if (len && !d) return;
    if (len) memcpy(d, s, len);
    n->data = d; n->size = len; n->cap = 0;        /* cap 0: fs.c never frees or grows it */
}

void procfs_gc(void)
{
    uint64_t now = kernel_ticks();
    fs_lock();
    struct node **pp = &made;
    while (*pp) {
        struct node *n = *pp;
        struct pent *p = n->priv;
        int busy = p->permanent || n->refs > 0 || now - p->born < 3 * AUXO_HZ;
        for (int i = 0; !busy && i < MAXTASK; i++) {
            struct task *o = task_slot(i);
            if (o->used && (o->ns.cwd.n == n || o->ns.root.n == n)) busy = 1;
        }
        if (busy) { pp = &p->gcnext; continue; }
        *pp = p->gcnext;
        if (n->parent && n->parent != n) n->parent->refs--;
        if (n->data) rs_kfree(n->data);
        rs_kfree(n->name);
        rs_kfree(p);
        rs_kfree(n);
    }
    fs_unlock();
}

static struct pidns *init_pidns(void);

struct node *procfs_new_root(struct pidns *ns)
{
    fs_lock();
    struct node *r = mk(0, "", N_DIR, 0555, P_ROOT, 0, 0, "");
    if (r) { pe(r)->ns = ns ? ns : init_pidns(); pe(r)->permanent = 1; }
    fs_unlock();
    return r;
}

/* ── processes ──────────────────────────────────────────────────────── */
static struct pidns *init_pidns(void)
{
    for (int i = 0; i < MAXTASK; i++) {                 /* any task's chain ends at the first namespace */
        struct task *o = task_slot(i);
        if (o->used && o->ns.pid) { struct pidns *p = o->ns.pid; while (p->parent) p = p->parent; return p; }
    }
    return 0;
}

static struct task *proc_task(int gpid)
{
    struct task *x = task_by_pid(gpid);
    return x && x->used && x->state != 2 ? x : 0;
}

/* the global tgid of process `vpid` as numbered in ns */
static int proc_lookup_pid(struct pidns *ns, int vpid)
{
    for (int i = 0; i < MAXTASK; i++) {
        struct task *o = task_slot(i);
        if (!o->used || o->is_thread || o->state == 2) continue;
        if (pid_in(ns, o, 1) == vpid) return o->tgid;
    }
    return 0;
}

static const char *const pid_entries[] = { "stat", "status", "maps", "cmdline", "comm", "environ", "mountinfo", "mounts",
    "cgroup", "oom_score_adj", "limits", "fd", "ns", "task", "cwd", "root", "exe", "statm", "loginuid", "sessionid", 0 };
static const char *const ns_entries[] = { "cgroup", "ipc", "mnt", "net", "pid", "pid_for_children", "uts", 0 };
static const char *const root_entries[] = { "self", "thread-self", "meminfo", "cpuinfo", "version", "filesystems", "mounts",
    "uptime", "loadavg", "stat", "sys", "kmsg", "cmdline", "swaps", "devices", 0 };

/* static directories under /proc/sys and their files */
static const char *const sys_tree[] = {
    "sys", "sys/kernel", "sys/kernel/random", "sys/fs", "sys/vm", "sys/net", "sys/net/core", "sys/user", 0 };
static const char *const sys_files[] = {
    "sys/kernel/osrelease", "sys/kernel/ostype", "sys/kernel/version", "sys/kernel/hostname", "sys/kernel/domainname",
    "sys/kernel/pid_max", "sys/kernel/ngroups_max", "sys/kernel/overflowuid", "sys/kernel/overflowgid", "sys/kernel/cap_last_cap",
    "sys/kernel/threads-max", "sys/kernel/random/boot_id", "sys/kernel/random/uuid", "sys/kernel/random/entropy_avail",
    "sys/kernel/yama", "sys/fs/file-max", "sys/fs/nr_open", "sys/fs/inotify", "sys/vm/overcommit_memory",
    "sys/vm/max_map_count", "sys/net/core/somaxconn", "sys/user/max_user_namespaces", 0 };

static int ns_index(const char *name) { for (int i = 0; ns_entries[i]; i++) if (!strcmp(ns_entries[i], name)) return i; return -1; }

static uint32_t ns_inum(struct task *x, int i)
{
    switch (i) {
    case 0: return x->ns.cgroup ? x->ns.cgroup->inum : 0;
    case 1: return x->ns.ipc ? x->ns.ipc->inum : 0;
    case 2: return x->ns.mnt ? x->ns.mnt->inum : 0;
    case 3: return x->ns.net ? x->ns.net->inum : 0;
    case 4: return x->ns.pid ? x->ns.pid->inum : 0;
    case 5: return x->ns.pid_children ? x->ns.pid_children->inum : x->ns.pid ? x->ns.pid->inum : 0;
    case 6: return x->ns.uts ? x->ns.uts->inum : 0;
    }
    return 0;
}

/* ── file contents ──────────────────────────────────────────────────── */
static char gen[32768];

/* CPU ticks a thread has run (pid -1: the idle thread) */
static uint64_t thread_ticks(int pid)
{
    for (size_t i = 0;; i++) {
        int p; uint64_t t; char nm[16];
        int st = rs_thread_stat(i, &p, &t, nm);
        if (st < 0) return 0;
        if (pid < 0) return i == 0 ? t : 0;
        if (st && p == pid) return t;
    }
}

static int text_pid(struct task *t, struct pidns *ns, struct task *x, const char *what, char *b, size_t cap)
{
    int n = 0;
    int vp = pid_in(ns, x, 1), vt = pid_in(ns, x, 0);
    struct task *par = x->ppid > 0 ? task_by_pid(x->ppid) : 0;
    int vpp = par ? pid_in(ns, par, 1) : 0;
    if (!strcmp(what, "stat")) {
        uint64_t ut = 0, rss = 0;
        for (int i = 0; i < MAXTASK; i++) { struct task *o = task_slot(i); if (o->used && (o == x || (!x->is_thread && o->tgid == x->tgid))) ut += thread_ticks(o->pid); }
        uint64_t f;
        __asm__ volatile("pushfq; pop %0; cli" : "=r"(f) :: "memory");
        rss = mm_rss(x->mm);
        if (f & 0x200) __asm__ volatile("sti");
        n = ksnprintf(b, cap, "%d (%s) %c %d %d %d 0 -1 4194560 0 0 0 0 %lu 0 0 0 20 %d %d 0 %lu 0 %lu 18446744073709551615 0 0 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0\n",
                      vt, x->name, x->state == 2 ? 'Z' : 'S', vpp, vp, vp, (unsigned long)ut, x->nice, 1, (unsigned long)x->start_tick, (unsigned long)rss);
    } else if (!strcmp(what, "status")) {
        int threads = 0;
        for (int i = 0; i < MAXTASK; i++) { struct task *o = task_slot(i); if (o->used && o->tgid == x->tgid) threads++; }
        n = ksnprintf(b, cap, "Name:\t%s\nUmask:\t0%c%c%c\nState:\t%s\nTgid:\t%d\nNgid:\t0\nPid:\t%d\nPPid:\t%d\nTracerPid:\t0\n"
                      "Uid:\t%u\t%u\t%u\t%u\nGid:\t%u\t%u\t%u\t%u\nFDSize:\t%d\nGroups:", x->name,
                      '0' + (int)((x->umask >> 6) & 7), '0' + (int)((x->umask >> 3) & 7), '0' + (int)(x->umask & 7),
                      x->state == 2 ? "Z (zombie)" : "S (sleeping)", vp, vt, vpp,
                      x->ns.cred.uid, x->ns.cred.euid, x->ns.cred.suid, x->ns.cred.fsuid,
                      x->ns.cred.gid, x->ns.cred.egid, x->ns.cred.sgid, x->ns.cred.fsgid, MAXFD);
        for (int g = 0; g < x->ns.cred.ngroups && g < 32; g++) n += ksnprintf(b + n, cap - (size_t)n, "%s%u", g ? " " : "\t", x->ns.cred.groups[g]);
        n += ksnprintf(b + n, cap - (size_t)n, "\nNStgid:");
        for (int l = ns ? ns->level : 0; l <= (x->ns.pid ? x->ns.pid->level : 0); l++) n += ksnprintf(b + n, cap - (size_t)n, "\t%d", x->ns.ltgid[l]);
        n += ksnprintf(b + n, cap - (size_t)n, "\nNSpid:");
        for (int l = ns ? ns->level : 0; l <= (x->ns.pid ? x->ns.pid->level : 0); l++) n += ksnprintf(b + n, cap - (size_t)n, "\t%d", x->ns.lpid[l]);
        n += ksnprintf(b + n, cap - (size_t)n, "\nNSpgid:\t%d\nNSsid:\t%d\nVmPeak:\t0 kB\nVmSize:\t0 kB\nVmRSS:\t0 kB\nThreads:\t%d\n"
                       "SigQ:\t0/0\nSigPnd:\t%016lx\nShdPnd:\t0000000000000000\nSigBlk:\t%016lx\nSigIgn:\t0000000000000000\nSigCgt:\t0000000000000000\n"
                       "CapInh:\t%016lx\nCapPrm:\t%016lx\nCapEff:\t%016lx\nCapBnd:\t%016lx\nCapAmb:\t%016lx\nNoNewPrivs:\t%d\nSeccomp:\t%d\nSeccomp_filters:\t%d\n"
                       "Cpus_allowed:\t1\nCpus_allowed_list:\t0\nvoluntary_ctxt_switches:\t0\nnonvoluntary_ctxt_switches:\t0\n",
                       vp, vp, threads, (unsigned long)x->pending, (unsigned long)x->sigmask,
                       (unsigned long)x->ns.cap_inh, (unsigned long)x->ns.cap_perm, (unsigned long)x->ns.cap_eff,
                       (unsigned long)x->ns.cap_bset, (unsigned long)x->ns.cap_amb, x->ns.no_new_privs, x->ns.seccomp_mode,
                       x->ns.filter ? 1 : 0);
    } else if (!strcmp(what, "maps")) {
        if (!x->mm) return 0;
        /* in address order, as Linux prints them: glibc's pthread_getattr_np works out the main
           thread's stack size from the line before the stack's */
        static int order[2048];
        int nv = x->mm->nv < 2048 ? x->mm->nv : 2048;
        for (int i = 0; i < nv; i++) {
            int j = i;
            while (j > 0 && x->mm->v[order[j - 1]].start > x->mm->v[i].start) { order[j] = order[j - 1]; j--; }
            order[j] = i;
        }
        for (int k = 0; k < nv && n < (int)cap - 300; k++) {
            struct vma *v = &x->mm->v[order[k]];
            char fp[200] = "";
            if (v->file && v->file->unlinked) ksnprintf(fp, sizeof fp, "/%s:%s (deleted)", v->file->name ? v->file->name : "memfd", "");
            else if (v->file) { struct path p = { 0, v->file }; vfs_pathname(t, p, fp, sizeof fp); }
            n += ksnprintf(b + n, cap - (size_t)n, "%lx-%lx %c%c%c%c %08lx 08:01 %u %s\n", v->start, v->end,
                           v->prot & 1 ? 'r' : '-', v->prot & 2 ? 'w' : '-', v->prot & 4 ? 'x' : '-', v->shared ? 's' : 'p', v->off,
                           v->file ? v->file->ino : 0, v->file ? fp : v->start >= 0x7FFFFF000000ULL ? "[stack]" : "");
        }
    } else if (!strcmp(what, "cmdline")) {
        n = ksnprintf(b, cap, "%s", x->exe[0] ? x->exe : x->name);
        n++;                                               /* NUL-terminated argument */
    } else if (!strcmp(what, "comm")) {
        n = ksnprintf(b, cap, "%s\n", x->name);
    } else if (!strcmp(what, "environ") || !strcmp(what, "loginuid") || !strcmp(what, "sessionid")) {
        n = !strcmp(what, "environ") ? 0 : ksnprintf(b, cap, "%s", !strcmp(what, "loginuid") ? "4294967295" : "4294967295");
    } else if (!strcmp(what, "mountinfo") || !strcmp(what, "mounts")) {
        n = mount_info(x, b, cap, what[5] == 0);
    } else if (!strcmp(what, "cgroup")) {
        n = ksnprintf(b, cap, "0::/\n");
    } else if (!strcmp(what, "oom_score_adj")) {
        n = ksnprintf(b, cap, "0\n");
    } else if (!strcmp(what, "statm")) {
        n = ksnprintf(b, cap, "0 0 0 0 0 0 0\n");
    } else if (!strcmp(what, "limits")) {
        n = ksnprintf(b, cap, "Limit                     Soft Limit           Hard Limit           Units     \n"
                              "Max open files            %-20d %-20d files     \n", MAXFD, MAXFD);
    }
    return n;
}

static int text_static(struct task *t, const char *path, char *b, size_t cap)
{
    if (!strcmp(path, "meminfo"))
        return ksnprintf(b, cap, "MemTotal:       %lu kB\nMemFree:        %lu kB\nMemAvailable:   %lu kB\nBuffers:        0 kB\nCached:         0 kB\n"
                         "SwapTotal:      0 kB\nSwapFree:       0 kB\n",
                         (unsigned long)(pmm_total_pages() * 4), (unsigned long)(pmm_free_pages() * 4), (unsigned long)(pmm_free_pages() * 4));
    if (!strcmp(path, "cpuinfo"))
        return ksnprintf(b, cap, "processor\t: 0\nvendor_id\t: GenuineIntel\nmodel name\t: %s\ncpu MHz\t\t: 2000.000\ncpu cores\t: 1\n"
                         "flags\t\t: fpu tsc cx8 cmov mmx fxsr sse sse2 sse3 ssse3 cx16 sse4_1 sse4_2 popcnt\n\n", cpu_brand());
    if (!strcmp(path, "version")) return ksnprintf(b, cap, "Linux version 6.8.0-auxobsd (AuxoBSD " AUXO_VERSION ") #1 SMP\n");
    if (!strcmp(path, "filesystems")) return ksnprintf(b, cap, "nodev\ttmpfs\nnodev\tproc\nnodev\tdevpts\nnodev\tmqueue\nnodev\tsysfs\n");
    if (!strcmp(path, "uptime")) { uint64_t s = kernel_ticks() / AUXO_HZ; return ksnprintf(b, cap, "%lu.%02lu %lu.00\n", (unsigned long)s, (unsigned long)(kernel_ticks() % AUXO_HZ), (unsigned long)s); }
    if (!strcmp(path, "loadavg")) return ksnprintf(b, cap, "0.00 0.00 0.00 1/%d 1\n", uproc_count());
    if (!strcmp(path, "stat")) {
        uint64_t idle = thread_ticks(-1), all = kernel_ticks(), busy = all > idle ? all - idle : 0;
        return ksnprintf(b, cap, "cpu  %lu 0 0 %lu 0 0 0 0 0 0\ncpu0 %lu 0 0 %lu 0 0 0 0 0 0\nbtime 1700000000\nprocesses %d\n",
                         (unsigned long)busy, (unsigned long)idle, (unsigned long)busy, (unsigned long)idle, uproc_count());
    }
    if (!strcmp(path, "cmdline")) return ksnprintf(b, cap, "%s\n", boot_cmdline());
    if (!strcmp(path, "swaps")) return ksnprintf(b, cap, "Filename\t\t\t\tType\t\tSize\t\tUsed\t\tPriority\n");
    if (!strcmp(path, "devices")) return ksnprintf(b, cap, "Character devices:\n  1 mem\n  4 tty\n  5 /dev/tty\n\nBlock devices:\n");
    if (!strcmp(path, "kmsg")) return 0;
    if (!strcmp(path, "sys/kernel/osrelease")) return ksnprintf(b, cap, "6.8.0-auxobsd\n");
    if (!strcmp(path, "sys/kernel/ostype")) return ksnprintf(b, cap, "Linux\n");
    if (!strcmp(path, "sys/kernel/version")) return ksnprintf(b, cap, "#1 SMP AuxoBSD " AUXO_VERSION "\n");
    if (!strcmp(path, "sys/kernel/hostname")) return ksnprintf(b, cap, "%s\n", uts_host(t));
    if (!strcmp(path, "sys/kernel/domainname")) return ksnprintf(b, cap, "(none)\n");
    if (!strcmp(path, "sys/kernel/pid_max")) return ksnprintf(b, cap, "4194304\n");
    if (!strcmp(path, "sys/kernel/ngroups_max")) return ksnprintf(b, cap, "65536\n");
    if (!strcmp(path, "sys/kernel/overflowuid") || !strcmp(path, "sys/kernel/overflowgid")) return ksnprintf(b, cap, "65534\n");
    if (!strcmp(path, "sys/kernel/cap_last_cap")) return ksnprintf(b, cap, "40\n");
    if (!strcmp(path, "sys/kernel/threads-max")) return ksnprintf(b, cap, "%d\n", MAXTASK);
    if (!strcmp(path, "sys/kernel/random/boot_id") || !strcmp(path, "sys/kernel/random/uuid")) {
        uint64_t x = rdtsc() * 0x9E3779B97F4A7C15ULL, y = x ^ (x >> 29) ^ 0xA5A5;
        return ksnprintf(b, cap, "%08x-%04x-4%03x-a%03x-%012lx\n", (unsigned)(x >> 32), (unsigned)(x & 0xFFFF), (unsigned)((x >> 16) & 0xFFF),
                         (unsigned)(y & 0xFFF), (unsigned long)(y >> 16) & 0xFFFFFFFFFFFFUL);
    }
    if (!strcmp(path, "sys/kernel/random/entropy_avail")) return ksnprintf(b, cap, "256\n");
    if (!strcmp(path, "sys/fs/file-max") || !strcmp(path, "sys/fs/nr_open")) return ksnprintf(b, cap, "%d\n", MAXFD);
    if (!strcmp(path, "sys/vm/overcommit_memory")) return ksnprintf(b, cap, "0\n");
    if (!strcmp(path, "sys/vm/max_map_count")) return ksnprintf(b, cap, "65530\n");
    if (!strcmp(path, "sys/net/core/somaxconn")) return ksnprintf(b, cap, "4096\n");
    if (!strcmp(path, "sys/user/max_user_namespaces")) return ksnprintf(b, cap, "0\n");   /* none: no user namespaces */
    return -1;
}

/* ── lookups ────────────────────────────────────────────────────────── */
static int is_num(const char *s) { if (!*s) return 0; for (; *s; s++) if (*s < '0' || *s > '9') return 0; return 1; }

static int in_list(const char *const *l, const char *s) { for (int i = 0; l[i]; i++) if (!strcmp(l[i], s)) return 1; return 0; }

static struct node *file_node(struct node *dir, const char *name, const char *text, int len)
{
    struct node *n = mk(dir, name, N_REG, 0444, P_FILE, 0, 0, 0);
    if (n && len > 0) set_text(n, text, (size_t)len);
    return n;
}

struct node *procfs_lookup(struct task *t, struct node *dir, const char *name)
{
    struct pent *d = pe(dir);
    if (!d) return 0;
    struct pidns *ns = d->ns;
    char sub[64];
    switch (d->kind) {
    case P_ROOT: {
        if (!strcmp(name, "self") || !strcmp(name, "thread-self")) {
            int me = t ? pid_in(ns, t, 1) : 0;
            if (!me) return 0;
            struct node *n = mk(dir, name, N_LNK, 0777, P_LINK, 0, 0, 0);
            if (!n) return 0;
            char txt[32];
            int l = name[0] == 's' ? ksnprintf(txt, sizeof txt, "%d", me) : ksnprintf(txt, sizeof txt, "%d/task/%d", me, pid_in(ns, t, 0));
            set_text(n, txt, (size_t)l);
            return n;
        }
        if (!strcmp(name, "mounts")) {
            struct node *n = mk(dir, name, N_LNK, 0777, P_LINK, 0, 0, 0);
            if (n) set_text(n, "self/mounts", 11);
            return n;
        }
        if (is_num(name)) {
            int g = proc_lookup_pid(ns, katoi(name));
            if (!g) return 0;
            return mk(dir, name, N_DIR, 0555, P_PID, g, 0, 0);
        }
        if (!strcmp(name, "sys")) return mk(dir, name, N_DIR, 0555, P_DIR, 0, 0, "sys");
        int l = text_static(t, name, gen, sizeof gen);
        if (l < 0 || !in_list(root_entries, name)) return 0;
        return file_node(dir, name, gen, l);
    }
    case P_DIR: {
        ksnprintf(sub, sizeof sub, "%s/%s", d->path, name);
        if (in_list(sys_tree, sub)) return mk(dir, name, N_DIR, 0555, P_DIR, 0, 0, sub);
        if (!in_list(sys_files, sub)) return 0;
        int l = text_static(t, sub, gen, sizeof gen);
        if (!strcmp(sub, "sys/kernel/yama") || !strcmp(sub, "sys/fs/inotify")) return mk(dir, name, N_DIR, 0555, P_DIR, 0, 0, sub);
        return l < 0 ? 0 : file_node(dir, name, gen, l);
    }
    case P_PID: case P_TASKDIR: {
        struct task *x = proc_task(d->pid);
        if (!x) return 0;
        if (d->kind == P_TASKDIR) {                      /* task/TID: the same entries (threads share them here) */
            if (!is_num(name)) return 0;
            int tid = katoi(name);
            for (int i = 0; i < MAXTASK; i++) {
                struct task *o = task_slot(i);
                if (o->used && o->tgid == x->tgid && pid_in(ns, o, 0) == tid) return mk(dir, name, N_DIR, 0555, P_PID, o->pid, 0, 0);
            }
            return 0;
        }
        if (!in_list(pid_entries, name)) return 0;
        if (!strcmp(name, "fd")) return mk(dir, name, N_DIR, 0500, P_FDDIR, d->pid, 0, 0);
        if (!strcmp(name, "ns")) return mk(dir, name, N_DIR, 0511, P_NSDIR, d->pid, 0, 0);
        if (!strcmp(name, "task")) return mk(dir, name, N_DIR, 0555, P_TASKDIR, d->pid, 0, 0);
        if (!strcmp(name, "cwd")) return mk(dir, name, N_LNK, 0777, P_CWD, d->pid, 0, 0);
        if (!strcmp(name, "root")) return mk(dir, name, N_LNK, 0777, P_ROOTLNK, d->pid, 0, 0);
        if (!strcmp(name, "exe")) return mk(dir, name, N_LNK, 0777, P_EXE, d->pid, 0, 0);
        /* maps and mountinfo grow with the process (a browser maps hundreds of libraries): size the text */
        size_t cap = sizeof gen;
        if (!strcmp(name, "maps") && x->mm) cap = (size_t)x->mm->nv * 256 + 4096;
        if (!strncmp(name, "mount", 5)) { cap = 4096; if (x->ns.mnt) for (struct mount *m = x->ns.mnt->mounts; m; m = m->next) cap += 700; }
        char *b = cap > sizeof gen ? rs_kmalloc(cap) : gen;
        if (!b) { b = gen; cap = sizeof gen; }
        int l = text_pid(t, ns, x, name, b, cap);
        struct node *n = file_node(dir, name, b, l);
        if (b != gen) rs_kfree(b);
        if (n && !strcmp(name, "oom_score_adj")) n->mode = 0644;
        return n;
    }
    case P_FDDIR: {
        struct task *x = proc_task(d->pid);
        if (!x || !x->files || !is_num(name)) return 0;
        int fd = katoi(name);
        if (fd < 0 || fd >= MAXFD || !x->files->fd[fd]) return 0;
        return mk(dir, name, N_LNK, 0700, P_FD, d->pid, fd, 0);
    }
    case P_NSDIR: {
        int i = ns_index(name);
        if (i < 0 || !proc_task(d->pid)) return 0;
        return mk(dir, name, N_LNK, 0777, P_NS, d->pid, i, 0);
    }
    }
    return 0;
}

/* directory listings (getdents); idx counts from 0 after "." and ".." */
int procfs_list(struct task *t, struct node *dir, uint64_t idx, char *name, size_t n, uint64_t *ino, uint8_t *type)
{
    struct pent *d = pe(dir);
    if (!d) return 0;
    uint64_t k = 0;
    *ino = 0x20000000 + idx;
    switch (d->kind) {
    case P_ROOT: {
        for (int i = 0; root_entries[i]; i++, k++)
            if (k == idx) { strlcpy(name, root_entries[i], n); *type = !strcmp(name, "sys") ? 4 : (!strcmp(name, "self") || !strcmp(name, "thread-self") || !strcmp(name, "mounts")) ? 10 : 8; return 1; }
        for (int i = 0; i < MAXTASK; i++) {
            struct task *o = task_slot(i);
            if (!o->used || o->is_thread || o->state == 2) continue;
            int v = pid_in(d->ns, o, 1);
            if (!v) continue;
            if (k++ == idx) { ksnprintf(name, n, "%d", v); *type = 4; *ino = 0x40000000u + (uint64_t)o->tgid; return 1; }
        }
        return 0;
    }
    case P_DIR: {
        size_t pl = strlen(d->path);
        for (int pass = 0; pass < 2; pass++) {
            const char *const *l = pass ? sys_files : sys_tree;
            for (int i = 0; l[i]; i++) {
                const char *s = l[i];
                if (strncmp(s, d->path, pl) || s[pl] != '/' || strchr(s + pl + 1, '/')) continue;
                if (k++ == idx) { strlcpy(name, s + pl + 1, n); *type = pass && strcmp(name, "yama") && strcmp(name, "inotify") ? 8 : 4; return 1; }
            }
        }
        return 0;
    }
    case P_PID: {
        for (int i = 0; pid_entries[i]; i++, k++)
            if (k == idx) {
                strlcpy(name, pid_entries[i], n);
                *type = (!strcmp(name, "fd") || !strcmp(name, "ns") || !strcmp(name, "task")) ? 4 :
                        (!strcmp(name, "cwd") || !strcmp(name, "root") || !strcmp(name, "exe")) ? 10 : 8;
                return 1;
            }
        return 0;
    }
    case P_TASKDIR: {
        struct task *x = proc_task(d->pid);
        if (!x) return 0;
        for (int i = 0; i < MAXTASK; i++) {
            struct task *o = task_slot(i);
            if (!o->used || o->tgid != x->tgid) continue;
            if (k++ == idx) { ksnprintf(name, n, "%d", pid_in(d->ns, o, 0)); *type = 4; return 1; }
        }
        return 0;
    }
    case P_FDDIR: {
        struct task *x = proc_task(d->pid);
        if (!x || !x->files) return 0;
        for (int fd = 0; fd < MAXFD; fd++) {
            if (!x->files->fd[fd]) continue;
            if (k++ == idx) { ksnprintf(name, n, "%d", fd); *type = 10; return 1; }
        }
        return 0;
    }
    case P_NSDIR:
        if (idx < 7) { strlcpy(name, ns_entries[idx], n); *type = 10; return 1; }
        return 0;
    }
    (void)t;
    return 0;
}

/* readlink on procfs's links */
int procfs_readlink(struct task *t, struct node *n, char *buf, size_t len)
{
    struct pent *p = pe(n);
    if (!p || n->type != N_LNK) return 0;
    if (p->kind == P_LINK) { size_t l = n->size < len - 1 ? (size_t)n->size : len - 1; memcpy(buf, n->data, l); buf[l] = 0; return 1; }
    struct task *x = proc_task(p->pid);
    if (!x) { strlcpy(buf, "", len); return 1; }
    switch (p->kind) {
    case P_FD: {
        struct file *f = x->files && p->fd < MAXFD ? x->files->fd[p->fd] : 0;
        if (!f) { buf[0] = 0; break; }
        if (f->node && !f->node->unlinked && f->kind == 0) { struct path q = { f->mnt, f->node }; vfs_pathname(t, q, buf, len); }
        else strlcpy(buf, f->path, len);
        break;
    }
    case P_CWD: vfs_pathname(t, x->ns.cwd, buf, len); break;
    case P_ROOTLNK: vfs_pathname(t, x->ns.root, buf, len); break;
    case P_EXE: if (x->ns.exe.n) vfs_pathname(t, x->ns.exe, buf, len); else strlcpy(buf, x->exe, len); break;
    case P_NS: ksnprintf(buf, len, "%s:[%u]", ns_entries[p->fd][3] == '_' ? "pid" : ns_entries[p->fd], ns_inum(x, p->fd)); break;
    }
    return 1;
}

/* following one of the magic links */
int procfs_magic(struct task *t, struct path at, struct path *out)
{
    struct pent *p = pe(at.n);
    (void)t;
    if (!p || at.n->type != N_LNK || p->kind == P_LINK) return 0;
    struct task *x = proc_task(p->pid);
    if (!x) return 0;
    switch (p->kind) {
    case P_FD: {
        struct file *f = x->files && p->fd < MAXFD ? x->files->fd[p->fd] : 0;
        if (!f || !f->node) return 0;
        out->m = f->mnt ? f->mnt : mount_rootm(); out->n = f->node;
        return 1;
    }
    case P_CWD: *out = x->ns.cwd; return out->n != 0;
    case P_ROOTLNK: *out = x->ns.root; return out->n != 0;
    case P_EXE: *out = x->ns.exe; return out->n != 0;
    case P_NS: {                                         /* an nsfs inode: stat shows the namespace id */
        struct node *n = mk(at.n, ns_entries[p->fd], N_REG, 0444, P_NSNODE, p->pid, p->fd, 0);
        if (!n) return 0;
        n->ino = ns_inum(x, p->fd);
        out->m = at.m; out->n = n;
        return 1;
    }
    }
    return 0;
}
