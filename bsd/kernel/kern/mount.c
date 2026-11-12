/* Mounts and mount namespaces (see ns.h): mount(2) with bind, recursive bind, remount and
 * tmpfs/proc/devpts, umount2, pivot_root, chroot, and the path walk that crosses mounts.
 * Every path a Linux program names goes through vfs_walk with its own namespace and root. */
#include "kern.h"
#include "arch.h"
#include "rs.h"
#include "fs.h"
#include "uproc.h"
#include "ns.h"

enum { EPERM = 1, ENOENT = 2, ENOMEM = 12, EFAULT = 14, EBUSY = 16, EEXIST = 17, ENODEV = 19, ENOTDIR = 20,
       EINVAL = 22, ENAMETOOLONG = 36, ELOOP = 40 };

#define MS_RDONLY 1
#define MS_REMOUNT 32
#define MS_BIND 4096
#define MS_MOVE 8192
#define MS_REC 16384
#define MS_UNBINDABLE (1 << 17)
#define MS_PRIVATE (1 << 18)
#define MS_SLAVE (1 << 19)
#define MS_SHARED (1 << 20)

static struct mntns init_ns;
static struct mount *init_root;
static int next_id = 21;

struct mntns *mntns_init(void) { return &init_ns; }
struct mount *mount_rootm(void) { return init_root; }
int vfs_readonly(struct mount *m) { return m && (m->flags & MNT_RDONLY); }

static struct mount *mount_new(struct mntns *ns, struct mount *parent, struct node *mp, struct node *root, int flags,
                               const char *type, const char *src)
{
    struct mount *m = rs_kmalloc(sizeof *m);
    if (!m) return 0;
    memset(m, 0, sizeof *m);
    m->parent = parent; m->mp = mp; m->root = root; m->flags = flags & MNT_PERMASK;
    m->id = next_id++;
    strlcpy(m->fstype, type ? type : "none", sizeof m->fstype);
    strlcpy(m->source, src && src[0] ? src : "none", sizeof m->source);
    m->ns = ns;
    struct mount **pp = &ns->mounts;
    while (*pp) pp = &(*pp)->next;
    *pp = m;
    if (root) { root->refs++; }
    return m;
}

/* the mount on top of node n as seen through mount pm (the newest, if several) */
static struct mount *mount_at(struct mntns *ns, struct mount *pm, struct node *n)
{
    struct mount *top = 0;
    for (struct mount *m = ns->mounts; m; m = m->next) if (m->parent == pm && m->mp == n) top = m;
    return top;
}

static void cross(struct mntns *ns, struct path *p)
{
    struct mount *c;
    for (int guard = 0; guard < 64 && (c = mount_at(ns, p->m, p->n)); guard++) { p->m = c; p->n = c->root; }
}

struct mount *mount_of_root(struct mntns *ns, struct node *n, struct mount *parent)
{
    for (struct mount *m = ns->mounts; m; m = m->next) if (m->root == n && (!parent || m->parent == parent)) return m;
    return 0;
}

void mount_init(void)
{
    init_ns.refs = 1;
    init_ns.inum = ns_inum_next();
    init_root = mount_new(&init_ns, 0, 0, fs_root(), 0, "rootfs", "rootfs");
    init_ns.root = init_root;
    struct node *proc = fs_child(fs_root(), "proc");
    struct node *pr = procfs_new_root(0);
    if (proc && pr) mount_new(&init_ns, init_root, proc, pr, MNT_NOSUID | MNT_NODEV | MNT_NOEXEC, "proc", "proc");
}

static struct path task_root(struct task *t)
{
    if (t && t->ns.root.m) return t->ns.root;
    struct path p = { init_root, fs_root() };
    return p;
}

static int same(struct path a, struct path b) { return a.m == b.m && a.n == b.n; }

/* ── the walk ───────────────────────────────────────────────────────── */
static int walk(struct task *t, struct path base, const char *path, int follow, struct path *out, int depth)
{
    if (depth > 40) return -ELOOP;
    struct mntns *ns = t && t->ns.mnt ? t->ns.mnt : &init_ns;
    struct path root = task_root(t);
    struct path cur = path[0] == '/' || !base.n ? root : base;
    if (!cur.m) cur.m = root.m;
    const char *p = path;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *e = p;
        while (*e && *e != '/') e++;
        size_t l = (size_t)(e - p);
        const char *after = e;
        while (*after == '/') after++;
        int last = !*after;
        int trailing = last && *e == '/';
        if (cur.n->type != N_DIR) return -ENOTDIR;
        if (l > 255) return -ENAMETOOLONG;
        struct path nx = cur;
        if (l == 1 && p[0] == '.') {
        } else if (l == 2 && p[0] == '.' && p[1] == '.') {
            struct path q = cur;
            while (!same(q, root) && q.n == q.m->root && q.m->parent) { q.n = q.m->mp; q.m = q.m->parent; }
            if (same(q, root) || (q.n == q.m->root && !q.m->parent) || q.n->parent == q.n) nx = q;
            else { nx.m = q.m; nx.n = q.n->parent; cross(ns, &nx); }
        } else {
            struct node *c;
            if (procfs_is(cur.n)) {
                char name[256];
                memcpy(name, p, l); name[l] = 0;
                c = procfs_lookup(t, cur.n, name);
            } else c = fs_child_n(cur.n, p, l);
            if (!c) return -ENOENT;
            nx.n = c;
            cross(ns, &nx);
        }
        if (nx.n->type == N_LNK && (!last || follow || trailing)) {
            struct path tgt;
            if (procfs_magic(t, nx, &tgt)) nx = tgt;              /* /proc/PID/fd/N etc.: the thing itself */
            else {
                char target[1024];
                size_t tl = nx.n->size < sizeof target - 1 ? (size_t)nx.n->size : sizeof target - 1;
                memcpy(target, nx.n->data, tl);
                target[tl] = 0;
                if (!tl) return -ENOENT;
                int r = walk(t, cur, target, 1, &nx, depth + 1);
                if (r < 0) return r;
            }
        }
        cur = nx;
        p = e;
    }
    *out = cur;
    return 0;
}

int vfs_walk(struct task *t, struct path base, const char *path, int follow, struct path *out)
{
    if (!path[0]) return -ENOENT;
    fs_lock();
    int r = walk(t, base, path, follow, out, 0);
    fs_unlock();
    return r;
}

int vfs_walk_parent(struct task *t, struct path base, const char *path, struct path *dir, char *name, size_t n)
{
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/') len--;
    size_t s = len;
    while (s > 0 && path[s - 1] != '/') s--;
    if (len == s) return len ? -21 : -ENOENT;                   /* "/" itself */
    if (len - s >= n) return -ENAMETOOLONG;
    memcpy(name, path + s, len - s);
    name[len - s] = 0;
    if (!strcmp(name, ".") || !strcmp(name, "..")) return -EBUSY;
    int r;
    if (s == 0) r = vfs_walk(t, base, ".", 1, dir);
    else {
        char dpath[1024];
        if (s >= sizeof dpath) return -ENAMETOOLONG;
        memcpy(dpath, path, s);
        dpath[s] = 0;
        r = vfs_walk(t, base, dpath, 1, dir);
    }
    if (r < 0) return r;
    if (dir->n->type != N_DIR) return -ENOTDIR;
    return 0;
}

/* the name of a path as its task sees it; 1 if it lies outside the task's root */
int vfs_pathname(struct task *t, struct path p, char *buf, size_t len)
{
    char tmp[1024];
    size_t pos = sizeof tmp - 1;
    tmp[pos] = 0;
    struct path root = task_root(t);
    int outside = 0;
    fs_lock();
    for (int guard = 0; guard < 4096 && p.n && !same(p, root); guard++) {
        if (p.m && p.n == p.m->root) {
            if (!p.m->parent) { outside = 1; break; }
            p.n = p.m->mp; p.m = p.m->parent;
            continue;
        }
        if (!p.n->parent || p.n->parent == p.n || !p.n->name) { outside = 1; break; }
        size_t l = strlen(p.n->name);
        if (l + 1 > pos) break;
        pos -= l;
        memcpy(tmp + pos, p.n->name, l);
        tmp[--pos] = '/';
        p.n = p.n->parent;
    }
    fs_unlock();
    if (pos == sizeof tmp - 1) tmp[--pos] = '/';
    strlcpy(buf, tmp + pos, len);
    return outside;
}

/* the path of a node inside its own filesystem (mountinfo's "root" column) */
static void fs_rel(struct node *n, char *buf, size_t len)
{
    char tmp[512];
    size_t pos = sizeof tmp - 1;
    tmp[pos] = 0;
    for (int guard = 0; guard < 1024 && n && n->parent && n->parent != n && n->name && pos > 1; guard++) {
        size_t l = strlen(n->name);
        if (l + 1 > pos) break;
        pos -= l;
        memcpy(tmp + pos, n->name, l);
        tmp[--pos] = '/';
        n = n->parent;
    }
    if (pos == sizeof tmp - 1) tmp[--pos] = '/';
    strlcpy(buf, tmp + pos, len);
}

/* ── namespaces ─────────────────────────────────────────────────────── */
static int is_under(struct node *n, struct node *top)
{
    for (int guard = 0; guard < 4096 && n; guard++) {
        if (n == top) return 1;
        if (!n->parent || n->parent == n) return 0;
        n = n->parent;
    }
    return 0;
}

struct mntns *mntns_copy(struct mntns *old, struct task *t)
{
    struct mntns *ns = rs_kmalloc(sizeof *ns);
    if (!ns) return 0;
    memset(ns, 0, sizeof *ns);
    ns->refs = 1;
    ns->inum = ns_inum_next();
    int n = 0;
    for (struct mount *m = old->mounts; m; m = m->next) n++;
    struct mount **from = rs_kmalloc(sizeof(void *) * (size_t)(n + 1) * 2), **to;
    if (!from) { rs_kfree(ns); return 0; }
    to = from + n + 1;
    fs_lock();
    int i = 0;
    for (struct mount *m = old->mounts; m; m = m->next, i++) {
        from[i] = m;
        to[i] = mount_new(ns, 0, m->mp, m->root, m->flags, m->fstype, m->source);
        if (!to[i]) { fs_unlock(); rs_kfree(from); return 0; }   /* (leaks the half-made copy) */
        to[i]->procns = m->procns;
    }
    for (i = 0; i < n; i++)
        for (int k = 0; k < n; k++) if (from[i]->parent == from[k]) { to[i]->parent = to[k]; break; }
    for (i = 0; i < n; i++) {
        if (from[i] == old->root) ns->root = to[i];
        if (t->ns.root.m == from[i]) t->ns.root.m = to[i];
        if (t->ns.cwd.m == from[i]) t->ns.cwd.m = to[i];
    }
    fs_unlock();
    rs_kfree(from);
    return ns;
}

void mntns_put(struct mntns *ns)
{
    if (ns && ns != &init_ns) ns->refs--;    /* mounts stay: open files and paths may point into them */
}

/* copy the mounts below (srcm, srcn) to below dst, keeping their places */
static void copy_tree(struct mntns *ns, struct mount *srcm, struct node *srcn, struct mount *dst, int depth)
{
    if (depth > 16) return;
    struct mount *snap[64];
    int n = 0;
    for (struct mount *m = ns->mounts; m && n < 64; m = m->next)
        if (m->parent == srcm && m != dst && (!srcn || is_under(m->mp, srcn))) snap[n++] = m;
    for (int i = 0; i < n; i++) {
        struct mount *c = mount_new(ns, dst, snap[i]->mp, snap[i]->root, snap[i]->flags, snap[i]->fstype, snap[i]->source);
        if (!c) return;
        c->procns = snap[i]->procns;
        copy_tree(ns, snap[i], 0, c, depth + 1);
    }
}

static void umount_tree(struct mntns *ns, struct mount *m)
{
    struct mount *kids[64];
    int n = 0;
    for (struct mount *c = ns->mounts; c && n < 64; c = c->next) if (c->parent == m) kids[n++] = c;
    for (int i = 0; i < n; i++) umount_tree(ns, kids[i]);
    struct mount **pp = &ns->mounts;
    while (*pp && *pp != m) pp = &(*pp)->next;
    if (*pp) *pp = m->next;
    m->next = 0;
    m->ns = 0;                       /* detached: paths that already point here keep working */
}

/* ── system calls ───────────────────────────────────────────────────── */
static long ustr(struct task *t, uint64_t u, char *buf, size_t n)
{
    if (!u) { buf[0] = 0; return 0; }
    long r = copyin_str(t->mm, buf, u, n);
    return r < 0 ? r : 0;
}

static int opt_mode(const char *data, int dflt)
{
    const char *m = data;
    while (m && *m) {
        if (!strncmp(m, "mode=", 5)) {
            int v = 0;
            for (m += 5; *m >= '0' && *m <= '7'; m++) v = v * 8 + (*m - '0');
            return v & 07777;
        }
        while (*m && *m != ',') m++;
        if (*m) m++;
    }
    return dflt;
}

long sys_mount(struct task *t, uint64_t usrc, uint64_t utgt, uint64_t utype, uint64_t flags, uint64_t udata)
{
    if (!ns_capable(t, CAP_SYS_ADMIN)) return -EPERM;
    char src[256], tgt[512], type[32], data[256];
    long r;
    if ((r = ustr(t, usrc, src, sizeof src)) < 0 || (r = ustr(t, utgt, tgt, sizeof tgt)) < 0 ||
        (r = ustr(t, utype, type, sizeof type)) < 0) return r;
    data[0] = 0;
    if (udata && copyin_str(t->mm, data, udata, sizeof data) < 0) data[0] = 0;
    if ((flags & 0xFFFF0000UL) == 0xC0ED0000UL) flags &= 0xFFFF;
    if (!tgt[0]) return -ENOENT;
    struct mntns *ns = t->ns.mnt;
    struct path tp;
    if ((r = vfs_walk(t, t->ns.cwd, tgt, 1, &tp)) < 0) return r;
    int per = (int)(flags & MNT_PERMASK);
    if (flags & MS_REMOUNT) {
        if (tp.n != tp.m->root) return -EINVAL;
        fs_lock();
        tp.m->flags = (tp.m->flags & ~MNT_PERMASK) | per;
        fs_unlock();
        return 0;
    }
    if (flags & (MS_SHARED | MS_PRIVATE | MS_SLAVE | MS_UNBINDABLE)) return 0;   /* nothing propagates here anyway */
    if (flags & (MS_BIND | MS_MOVE)) {
        struct path sp;
        if (!src[0]) return -EINVAL;
        if ((r = vfs_walk(t, t->ns.cwd, src, 1, &sp)) < 0) return r;
        if ((sp.n->type == N_DIR) != (tp.n->type == N_DIR)) return -ENOTDIR;
        fs_lock();
        if (flags & MS_MOVE) {
            struct mount *m = sp.m;
            r = 0;
            if (sp.n != m->root || !m->parent || m->ns != ns) r = -EINVAL;
            for (struct mount *a = tp.m; a && !r; a = a->parent) if (a == m) r = -ELOOP;   /* into itself */
            if (!r) { m->parent = tp.m; m->mp = tp.n; }
            fs_unlock();
            return r;
        }
        struct mount *nm = mount_new(ns, tp.m, tp.n, sp.n, sp.m->flags, sp.m->fstype, sp.m->source);
        if (nm) {
            nm->procns = sp.m->procns;
            if (flags & MS_REC) copy_tree(ns, sp.m, sp.n, nm, 0);
        }
        fs_unlock();
        return nm ? 0 : -ENOMEM;
    }
    struct node *root = 0;
    struct pidns *procns = 0;
    if (!strcmp(type, "tmpfs") || !strcmp(type, "ramfs") || !strcmp(type, "mqueue") || !strcmp(type, "sysfs")) {
        root = fs_new_root((uint16_t)opt_mode(data, 01777));
    } else if (!strcmp(type, "devpts")) {
        root = fs_new_root(0755);
    } else if (!strcmp(type, "proc")) {
        procns = t->ns.pid;
        root = procfs_new_root(procns);
    } else return -ENODEV;
    if (!root) return -ENOMEM;
    if (tp.n->type != N_DIR) return -ENOTDIR;
    fs_lock();
    struct mount *nm = mount_new(ns, tp.m, tp.n, root, per, type, src[0] ? src : type);
    if (nm) nm->procns = procns;
    fs_unlock();
    return nm ? 0 : -ENOMEM;
}

long sys_umount2(struct task *t, uint64_t utgt, long flags)
{
    if (!ns_capable(t, CAP_SYS_ADMIN)) return -EPERM;
    if (flags & ~0xFL) return -EINVAL;
    char tgt[512];
    long r;
    if ((r = ustr(t, utgt, tgt, sizeof tgt)) < 0) return r;
    struct path p;
    if ((r = vfs_walk(t, t->ns.cwd, tgt, !(flags & 8), &p)) < 0) return r;
    fs_lock();
    cross(t->ns.mnt, &p);                                    /* what's mounted here, not what's under it */
    struct mount *m = p.m;
    r = 0;
    if (p.n != m->root || !m->parent || m->ns != t->ns.mnt) r = -EINVAL;
    else umount_tree(t->ns.mnt, m);
    fs_unlock();
    return r;
}

long sys_pivot_root(struct task *t, uint64_t unew, uint64_t uold)
{
    if (!ns_capable(t, CAP_SYS_ADMIN)) return -EPERM;
    char np_[512], op_[512];
    long r;
    if ((r = ustr(t, unew, np_, sizeof np_)) < 0 || (r = ustr(t, uold, op_, sizeof op_)) < 0) return r;
    struct path np, op;
    if ((r = vfs_walk(t, t->ns.cwd, np_, 1, &np)) < 0) return r;
    if ((r = vfs_walk(t, t->ns.cwd, op_, 1, &op)) < 0) return r;
    if (np.n->type != N_DIR || op.n->type != N_DIR) return -ENOTDIR;
    struct mntns *ns = t->ns.mnt;
    struct path root = task_root(t);
    fs_lock();
    struct mount *R = root.m, *N = np.m;
    r = 0;
    if (np.n != N->root || root.n != R->root || N == R || N->ns != ns || R->ns != ns) r = -EINVAL;
    /* put_old has to be at or under the new root */
    if (!r) {
        struct path q = op;
        int ok = 0;
        for (int guard = 0; guard < 4096; guard++) {
            if (same(q, np)) { ok = 1; break; }
            if (q.n == q.m->root) { if (!q.m->parent) break; q.n = q.m->mp; q.m = q.m->parent; continue; }
            if (q.n->parent == q.n) break;
            q.n = q.n->parent;
        }
        if (!ok) r = -EINVAL;
    }
    if (!r) {
        N->parent = 0; N->mp = 0;
        R->parent = op.m; R->mp = op.n;
        if (ns->root == R) ns->root = N;
        for (int i = 0; i < MAXTASK; i++) {
            struct task *o = task_slot(i);
            if (!o->used || o->ns.mnt != ns) continue;
            if (o->ns.root.m == R && o->ns.root.n == R->root) { o->ns.root.m = N; o->ns.root.n = N->root; }
            if (o->ns.cwd.m == R && o->ns.cwd.n == R->root) { o->ns.cwd.m = N; o->ns.cwd.n = N->root; }
        }
    }
    fs_unlock();
    return r;
}

long sys_chroot(struct task *t, uint64_t upath)
{
    if (!ns_capable(t, CAP_SYS_CHROOT)) return -EPERM;
    char p_[512];
    long r;
    if ((r = ustr(t, upath, p_, sizeof p_)) < 0) return r;
    struct path p;
    if ((r = vfs_walk(t, t->ns.cwd, p_, 1, &p)) < 0) return r;
    if (p.n->type != N_DIR) return -ENOTDIR;
    t->ns.root = p;
    return 0;
}

/* ── /proc/self/mountinfo and /proc/self/mounts ─────────────────────── */
static size_t esc(char *o, size_t cap, const char *s)
{
    size_t n = 0;
    for (; *s && n + 5 < cap; s++) {
        if (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\\') {
            unsigned char c = (unsigned char)*s;
            o[n++] = '\\'; o[n++] = (char)('0' + (c >> 6)); o[n++] = (char)('0' + ((c >> 3) & 7)); o[n++] = (char)('0' + (c & 7));
        }
        else o[n++] = *s;
    }
    o[n] = 0;
    return n;
}

int mount_info(struct task *t, char *buf, size_t cap, int mtab)
{
    struct mntns *ns = t && t->ns.mnt ? t->ns.mnt : &init_ns;
    size_t n = 0;
    for (struct mount *m = ns->mounts; m && n + 600 < cap; m = m->next) {
        char mp[512], rt[256], e1[600], e2[300], opts[64];
        struct path at = { m, m->root };
        if (vfs_pathname(t, at, mp, sizeof mp)) continue;      /* not under this task's root */
        fs_rel(m->root, rt, sizeof rt);
        ksnprintf(opts, sizeof opts, "%s%s%s%s,relatime", m->flags & MNT_RDONLY ? "ro" : "rw", m->flags & MNT_NOSUID ? ",nosuid" : "",
                  m->flags & MNT_NODEV ? ",nodev" : "", m->flags & MNT_NOEXEC ? ",noexec" : "");
        esc(e1, sizeof e1, mp);
        esc(e2, sizeof e2, rt);
        if (mtab) n += (size_t)ksnprintf(buf + n, cap - n, "%s %s %s %s 0 0\n", m->source, e1, m->fstype, opts);
        else n += (size_t)ksnprintf(buf + n, cap - n, "%d %d 0:%d %s %s %s - %s %s %s\n", m->id, m->parent ? m->parent->id : m->id,
                                    m->id, e2, e1, opts, m->fstype, m->source, m->flags & MNT_RDONLY ? "ro" : "rw");
    }
    return (int)n;
}
