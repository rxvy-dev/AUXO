/* The Linux x86_64 system-call interface. Numbers, structures and errno values are Linux's,
 * so unmodified programs (glibc, musl, BusyBox, GCC …) run. */
#include "kern.h"
#include "arch.h"
#include "rs.h"
#include "fs.h"
#include "uproc.h"
#include "auxo.h"
#include "unix.h"
#include "inet.h"
#include "install.h"
#include "net.h"
#include "netlink.h"

struct task *task_alloc(void);
void task_free(struct task *t);
struct task *task_slot(int i);
void uproc_set_fg(int pgid);
int pipe_readable(struct file *f);
int pipe_writable(struct file *f);
extern int tty_icanon, tty_echo;
void task_exit(struct task *t, int status) __attribute__((noreturn));
static void exit_group(struct task *t, int status) __attribute__((noreturn));

#define E(x) (-(long)(x))
enum { EPERM = 1, ENOENT, ESRCH, EINTR, EIO, ENXIO, E2BIG, ENOEXEC, EBADF, ECHILD, EAGAIN, ENOMEM, EACCES, EFAULT,
       EBUSY = 16, EEXIST, EXDEV, ENODEV, ENOTDIR, EISDIR, EINVAL, ENFILE, EMFILE, ENOTTY, ESPIPE = 29, EROFS, EMLINK,
       EPIPE, ERANGE = 34, ENAMETOOLONG = 36, ENOSYS = 38, ENOTEMPTY = 39, ELOOP = 40, ENOTSOCK = 88, EAFNOSUPPORT = 97,
       EOPNOTSUPP = 95, EXDEV_ = 18, ENETUNREACH = 101, ECONNREFUSED = 111 };

#define AT_FDCWD -100
#define EMSGSIZE_ 90
#define ENOPROTOOPT_ 92

static uint64_t rdmsr(uint32_t m) { uint32_t lo, hi; __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(m)); return (uint64_t)hi << 32 | lo; }
static void wrmsr(uint32_t m, uint64_t v) { __asm__ volatile("wrmsr" :: "c"(m), "a"((uint32_t)v), "d"((uint32_t)(v >> 32))); }

/* ── helpers ────────────────────────────────────────────────────────── */
static struct file *getf(struct task *t, long fd)
{
    if (fd < 0 || fd >= MAXFD) return 0;
    return t->files->fd[fd];
}

static int alloc_fd(struct task *t, struct file *f, int from, int cloexec)
{
    for (int i = from; i < MAXFD; i++)
        if (!t->files->fd[i]) { t->files->fd[i] = f; t->files->cloexec[i] = (uint8_t)cloexec; return i; }
    return -EMFILE;
}

static long upath(struct task *t, uint64_t up, char *buf, size_t n)
{
    long r = copyin_str(t->mm, buf, up, n);
    return r < 0 ? r : 0;
}

/* the directory a *at() call is relative to */
static int at_dir(struct task *t, long dirfd, const char *path, struct path *dir)
{
    if (path[0] == '/' || dirfd == AT_FDCWD) { *dir = t->ns.cwd; return 0; }
    struct file *f = getf(t, dirfd);
    if (!f || !f->node) return -EBADF;
    if (f->node->type != N_DIR) return -ENOTDIR;
    dir->m = f->mnt ? f->mnt : mount_rootm();
    dir->n = f->node;
    return 0;
}

/* resolve a *at() path: 0 or -errno */
static int at_walk(struct task *t, long dirfd, const char *path, int follow, struct path *out)
{
    struct path dir;
    int r = at_dir(t, dirfd, path, &dir);
    return r < 0 ? r : vfs_walk(t, dir, path, follow, out);
}

static int at_parent(struct task *t, long dirfd, const char *path, struct path *pd, char *name, size_t n)
{
    struct path dir;
    int r = at_dir(t, dirfd, path, &dir);
    if (r < 0) return r;
    if ((r = vfs_walk_parent(t, dir, path, pd, name, n)) < 0) return r;
    if (vfs_readonly(pd->m)) return -EROFS;
    if (procfs_is(pd->n)) return -EACCES;
    return 0;
}

static int procfile(struct node *n) { return n && n->dev == 0x50524F43u && n->type != N_CHR; }

static uint32_t mode_of(struct node *n)
{
    uint32_t m = n->mode & 07777;
    switch (n->type) {
    case N_DIR: return m | 0040000;
    case N_LNK: return 0777 | 0120000;
    case N_CHR: return m | 0020000;
    case N_FIFO: return m | 0010000;
    case N_SOCK: return m | 0140000;
    default: return m | 0100000;
    }
}

struct kstat {
    uint64_t dev, ino, nlink; uint32_t mode, uid, gid, pad; uint64_t rdev; int64_t size, blksize, blocks;
    int64_t atime, atime_ns, mtime, mtime_ns, ctime, ctime_ns; int64_t reserved[3];
};

static void fill_stat(struct node *n, struct kstat *s)
{
    memset(s, 0, sizeof *s);
    s->dev = 0x801; s->ino = n->ino; s->nlink = n->type == N_DIR ? 2 : n->nlink; s->mode = mode_of(n);
    s->rdev = n->type == N_CHR ? (n->dev == DEV_TTY ? 0x500 : 0x100 + n->dev) : 0;
    s->size = (int64_t)n->size; s->blksize = 4096; s->blocks = (int64_t)((n->size + 511) / 512);
    s->atime = s->mtime = s->ctime = n->mtime ? n->mtime : 1700000000;
}

static long do_stat_node(struct task *t, struct node *n, uint64_t ubuf)
{
    struct kstat s;
    fill_stat(n, &s);
    return copyout(t->mm, ubuf, &s, sizeof s);
}

static long do_statx(struct task *t, struct node *n, uint64_t ubuf)
{
    uint8_t b[256];
    memset(b, 0, sizeof b);
    struct kstat s;
    fill_stat(n, &s);
    uint32_t mask = 0x7FF, blk = 4096, nl = (uint32_t)s.nlink, z = 0;
    uint16_t mode = (uint16_t)s.mode;
    memcpy(b + 0, &mask, 4); memcpy(b + 4, &blk, 4); memcpy(b + 16, &nl, 4); memcpy(b + 20, &z, 4); memcpy(b + 24, &z, 4);
    memcpy(b + 28, &mode, 2); memcpy(b + 32, &s.ino, 8); memcpy(b + 40, &s.size, 8); memcpy(b + 48, &s.blocks, 8);
    for (int k = 0; k < 4; k++) memcpy(b + 64 + k * 16, &s.mtime, 8);
    uint32_t maj = 8, min = 1;
    memcpy(b + 136, &maj, 4); memcpy(b + 140, &min, 4);
    return copyout(t->mm, ubuf, b, sizeof b);
}

/* ── open / close / read / write ────────────────────────────────────── */
static long sys_openat(struct task *t, long dirfd, uint64_t up, long flags, long mode)
{
    char path[1024];
    long r = upath(t, up, path, sizeof path);
    if (r < 0) return r;
    struct path p;
    int wr = (flags & 3) == 1 || (flags & 3) == 2 || (flags & 01000);
    r = at_walk(t, dirfd, path, !(flags & 0400000), &p);
    if (r == -ENOENT && (flags & 0100)) {                  /* O_CREAT */
        struct path pd; char name[256];
        if ((r = at_parent(t, dirfd, path, &pd, name, sizeof name)) < 0) return r;
        struct node *n = fs_create(pd.n, name, N_REG, (uint16_t)(mode & ~t->umask & 07777));
        if (!n) return -ENOMEM;
        p.m = pd.m; p.n = n;
        r = 0;
    } else if (r == 0 && (flags & 0100) && (flags & 0200)) return -EEXIST;
    if (r < 0) return r;
    struct node *n = p.n;
    if ((flags & 0200000) && n->type != N_DIR) return -ENOTDIR;
    if (n->type == N_DIR && (flags & 3)) return -EISDIR;
    if (n->type == N_LNK && (flags & 0400000) && !(flags & 010000000)) return -ELOOP;
    if (wr && n->type != N_CHR && n->type != N_FIFO && n->type != N_SOCK && vfs_readonly(p.m)) return -EROFS;
    if (wr && procfile(n) && !(n->mode & 0200)) return -EACCES;
    if (n->type == N_CHR && p.m && (p.m->flags & MNT_NODEV) && !(flags & 010000000)) return -EACCES;
    if ((flags & 01000) && n->type == N_REG && (flags & 3)) fs_truncate(n, 0);
    struct file *f = file_new(0, n, (int)flags);
    if (!f) return -ENOMEM;
    f->mnt = p.m;
    vfs_pathname(t, p, f->path, sizeof f->path);
    int fd = alloc_fd(t, f, 0, (flags & 02000000) != 0);
    if (fd < 0) file_put(f);
    return fd;
}

static long sys_close(struct task *t, long fd)
{
    struct file *f = getf(t, fd);
    if (!f) return -EBADF;
    t->files->fd[fd] = 0;
    file_put(f);
    return 0;
}

#define KBUF 65536
static uint8_t *kbuf_get(void) { return rs_kmalloc(KBUF); }

static long sys_read(struct task *t, long fd, uint64_t ubuf, uint64_t n)
{
    struct file *f = getf(t, fd);
    if (!f) return -EBADF;
    if (f->node && f->node->type == N_DIR) return -EISDIR;
    /* regular files: copy straight from the file's data */
    if (f->kind == 0 && f->node && f->node->type == N_REG) {
        struct node *nd = f->node;
        if (f->pos >= nd->size) return 0;
        if (n > nd->size - f->pos) n = nd->size - f->pos;
        if (copyout(t->mm, ubuf, nd->data + f->pos, n) < 0) return -EFAULT;
        f->pos += n;
        return (long)n;
    }
    uint8_t *kb = kbuf_get();
    if (!kb) return -ENOMEM;
    /* /dev/zero and /dev/urandom fill the whole read, as on Linux (dd counts on it) */
    int full = f->node && f->node->type == N_CHR && (f->node->dev == DEV_ZERO || f->node->dev == DEV_RANDOM);
    long total = 0;
    do {
        size_t c = n - (uint64_t)total > KBUF ? KBUF : (size_t)(n - (uint64_t)total);
        long r = file_read(t, f, kb, c);
        if (r > 0 && copyout(t->mm, ubuf + (uint64_t)total, kb, (size_t)r) < 0) r = -EFAULT;
        if (r <= 0) { if (!total) total = r; break; }
        total += r;
    } while (full && (uint64_t)total < n);
    rs_kfree(kb);
    return total;
}

static long sys_write(struct task *t, long fd, uint64_t ubuf, uint64_t n)
{
    struct file *f = getf(t, fd);
    if (!f) return -EBADF;
    uint8_t *kb = kbuf_get();
    if (!kb) return -ENOMEM;
    long total = 0;
    while (n) {
        size_t c = n > KBUF ? KBUF : n;
        if (copyin(t->mm, kb, ubuf, c) < 0) { total = total ? total : -EFAULT; break; }
        long r = file_write(t, f, kb, c);
        if (r < 0) { total = total ? total : r; break; }
        total += r; ubuf += (uint64_t)r; n -= (uint64_t)r;
        if ((size_t)r < c) break;
    }
    rs_kfree(kb);
    if (total == -EPIPE) t->pending |= 1ULL << 12;          /* SIGPIPE */
    return total;
}

static long sys_rwv(struct task *t, long fd, uint64_t uiov, long cnt, int write, int64_t off)
{
    struct file *f = getf(t, fd);
    if (!f) return -EBADF;
    uint64_t save = f->pos;
    if (off >= 0) f->pos = (uint64_t)off;
    long total = 0;
    for (long i = 0; i < cnt && i < 1024; i++) {
        uint64_t iov[2];
        if (copyin(t->mm, iov, uiov + (uint64_t)i * 16, 16) < 0) { total = -EFAULT; break; }
        if (!iov[1]) continue;
        long r = write ? sys_write(t, fd, iov[0], iov[1]) : sys_read(t, fd, iov[0], iov[1]);
        if (r < 0) { if (!total) total = r; break; }
        total += r;
        if ((uint64_t)r < iov[1]) break;
    }
    if (off >= 0) f->pos = save;
    return total;
}

static long sys_pread(struct task *t, long fd, uint64_t ubuf, uint64_t n, int64_t off, int write)
{
    struct file *f = getf(t, fd);
    if (!f) return -EBADF;
    if (!f->node || f->node->type != N_REG) return -ESPIPE;
    uint64_t save = f->pos;
    f->pos = (uint64_t)off;
    long r = write ? sys_write(t, fd, ubuf, n) : sys_read(t, fd, ubuf, n);
    f->pos = save;
    return r;
}

static long sys_lseek(struct task *t, long fd, int64_t off, long whence)
{
    struct file *f = getf(t, fd);
    if (!f) return -EBADF;
    if (f->kind) return -ESPIPE;
    int64_t base = whence == 0 ? 0 : whence == 1 ? (int64_t)f->pos : whence == 2 ? (int64_t)f->node->size : -1;
    if (base < 0 || base + off < 0) return -EINVAL;
    f->pos = (uint64_t)(base + off);
    return (long)f->pos;
}

static long sys_getdents64(struct task *t, long fd, uint64_t ubuf, uint64_t n)
{
    struct file *f = getf(t, fd);
    if (!f || !f->node) return -EBADF;
    if (f->node->type != N_DIR) return -ENOTDIR;
    uint8_t *kb = kbuf_get();
    if (!kb) return -ENOMEM;
    if (n > KBUF) n = KBUF;
    size_t o = 0;
    fs_lock();
    struct node *c = f->node->child;
    uint64_t idx = 0;
    int proc = procfs_is(f->node);
    char pname[64];
    for (;;) {
        const char *name; uint64_t ino; uint8_t type;
        if (idx == 0) { name = "."; ino = f->node->ino; type = 4; }
        else if (idx == 1) { name = ".."; ino = f->node->parent->ino; type = 4; }
        else if (proc) {
            if (!procfs_list(t, f->node, idx - 2, pname, sizeof pname, &ino, &type)) break;
            name = pname;
        } else {
            uint64_t k = idx - 2;
            c = f->node->child;
            while (c && k) { c = c->next; k--; }
            if (!c) break;
            name = c->name; ino = c->ino;
            type = c->type == N_DIR ? 4 : c->type == N_LNK ? 10 : c->type == N_CHR ? 2 : 8;
        }
        if (idx < f->pos) { idx++; continue; }
        size_t nl = strlen(name), rec = (19 + nl + 1 + 7) & ~7UL;
        if (o + rec > n) break;
        uint64_t next = idx + 1;
        uint16_t rl = (uint16_t)rec;
        memcpy(kb + o, &ino, 8); memcpy(kb + o + 8, &next, 8); memcpy(kb + o + 16, &rl, 2);
        kb[o + 18] = type;
        memcpy(kb + o + 19, name, nl + 1);
        o += rec;
        idx++;
        f->pos = idx;
    }
    fs_unlock();
    long r = (o == 0 && f->pos < idx) ? -EINVAL : (long)o;
    if (o && copyout(t->mm, ubuf, kb, o) < 0) r = -EFAULT;
    rs_kfree(kb);
    return r;
}

/* ── paths ──────────────────────────────────────────────────────────── */
static long sys_fstatat(struct task *t, long dirfd, uint64_t up, uint64_t ubuf, long flags, int x)
{
    char path[1024];
    long r;
    struct node *n, *dir;
    if ((r = upath(t, up, path, sizeof path)) < 0) return r;
    if (!path[0] && (flags & 0x1000)) {                    /* AT_EMPTY_PATH: the fd itself */
        struct file *f = getf(t, dirfd);
        if (!f) return -EBADF;
        if (f->kind == 1 || f->kind == 2) {
            struct kstat s; memset(&s, 0, sizeof s); s.mode = 0010600; s.ino = 9999; s.blksize = 4096;
            if (x) { struct node tmp; memset(&tmp, 0, sizeof tmp); tmp.type = N_FIFO; tmp.mode = 0600; return do_statx(t, &tmp, ubuf); }
            return copyout(t->mm, ubuf, &s, sizeof s);
        }
        n = f->node;
    } else {
        struct path p;
        (void)dir;
        if ((r = at_walk(t, dirfd, path, !(flags & 0x100), &p)) < 0) return r;
        n = p.n;
    }
    return x ? do_statx(t, n, ubuf) : do_stat_node(t, n, ubuf);
}

static long sys_readlinkat(struct task *t, long dirfd, uint64_t up, uint64_t ubuf, uint64_t sz)
{
    char path[1024], out[512];
    long r;
    if ((r = upath(t, up, path, sizeof path)) < 0) return r;
    {
        struct path p;
        if ((r = at_walk(t, dirfd, path, 0, &p)) < 0) return r;
        struct node *n = p.n;
        if (n->type != N_LNK) return -EINVAL;
        if (!procfs_readlink(t, n, out, sizeof out)) {
            size_t l = n->size < sizeof out - 1 ? (size_t)n->size : sizeof out - 1;
            memcpy(out, n->data, l); out[l] = 0;
        }
    }
    size_t l = strlen(out);
    if (l > sz) l = sz;
    if (copyout(t->mm, ubuf, out, l) < 0) return -EFAULT;
    return (long)l;
}

static long sys_faccessat(struct task *t, long dirfd, uint64_t up, long mode)
{
    char path[1024];
    long r;
    struct path p;
    if ((r = upath(t, up, path, sizeof path)) < 0) return r;
    if ((r = at_walk(t, dirfd, path, 1, &p)) < 0) return r;
    struct node *n = p.n;
    if ((mode & 1) && n->type == N_REG && !(n->mode & 0111)) return -EACCES;
    if ((mode & 2) && vfs_readonly(p.m) && n->type != N_CHR && n->type != N_SOCK && n->type != N_FIFO) return -EROFS;
    if ((mode & 2) && procfile(n) && !(n->mode & 0200)) return -EACCES;
    return 0;
}

static long sys_mkdirat(struct task *t, long dirfd, uint64_t up, long mode)
{
    char path[1024], name[256];
    long r;
    struct path pd;
    if ((r = upath(t, up, path, sizeof path)) < 0) return r;
    if ((r = at_parent(t, dirfd, path, &pd, name, sizeof name)) < 0) return r == -EBUSY ? -EEXIST : r;
    if (fs_child(pd.n, name)) return -EEXIST;
    return fs_create(pd.n, name, N_DIR, (uint16_t)(mode & ~t->umask & 07777)) ? 0 : -ENOMEM;
}

static long sys_unlinkat(struct task *t, long dirfd, uint64_t up, long flags)
{
    char path[1024], name[256];
    long r;
    struct path pd;
    if ((r = upath(t, up, path, sizeof path)) < 0) return r;
    if ((r = at_parent(t, dirfd, path, &pd, name, sizeof name)) < 0) return r;
    return fs_unlink(pd.n, name, (flags & 0x200) != 0);
}

static long sys_renameat(struct task *t, long ofd, uint64_t uo, long nfd, uint64_t un)
{
    char op[1024], np[1024], on[256], nn[256];
    long r;
    struct path opd, npd;
    if ((r = upath(t, uo, op, sizeof op)) < 0 || (r = upath(t, un, np, sizeof np)) < 0) return r;
    if ((r = at_parent(t, ofd, op, &opd, on, sizeof on)) < 0) return r;
    if ((r = at_parent(t, nfd, np, &npd, nn, sizeof nn)) < 0) return r;
    if (opd.m != npd.m) return -EXDEV_;                    /* different mounts, even of the same tree */
    return fs_rename(opd.n, on, npd.n, nn);
}

static long sys_symlinkat(struct task *t, uint64_t ut, long dirfd, uint64_t up)
{
    char target[1024], path[1024], name[256];
    long r;
    struct path pd;
    if ((r = upath(t, ut, target, sizeof target)) < 0 || (r = upath(t, up, path, sizeof path)) < 0) return r;
    if ((r = at_parent(t, dirfd, path, &pd, name, sizeof name)) < 0) return r == -EBUSY ? -EEXIST : r;
    if (fs_child(pd.n, name)) return -EEXIST;
    struct node *n = fs_create(pd.n, name, N_LNK, 0777);
    if (!n) return -ENOMEM;
    return fs_write(n, 0, target, strlen(target)) < 0 ? -ENOMEM : 0;
}

static long sys_linkat(struct task *t, long ofd, uint64_t uo, long nfd, uint64_t un)
{
    char op[1024], np[1024], nn[256];
    long r;
    struct path src, npd;
    if ((r = upath(t, uo, op, sizeof op)) < 0 || (r = upath(t, un, np, sizeof np)) < 0) return r;
    if ((r = at_walk(t, ofd, op, 0, &src)) < 0) return r;
    if ((r = at_parent(t, nfd, np, &npd, nn, sizeof nn)) < 0) return r;
    if (src.m != npd.m) return -EXDEV_;
    return fs_link(src.n, npd.n, nn);
}

static long sys_chdir(struct task *t, uint64_t up)
{
    char path[1024];
    long r;
    struct path p;
    if ((r = upath(t, up, path, sizeof path)) < 0) return r;
    if ((r = vfs_walk(t, t->ns.cwd, path, 1, &p)) < 0) return r;
    if (p.n->type != N_DIR) return -ENOTDIR;
    t->ns.cwd = p;
    t->cwd = p.n;
    return 0;
}

static long sys_getcwd(struct task *t, uint64_t ubuf, uint64_t sz)
{
    char p[1024];
    if (vfs_pathname(t, t->ns.cwd, p, sizeof p)) {             /* outside our root (after pivot_root) */
        char q[1024]; ksnprintf(q, sizeof q, "(unreachable)%s", p); strlcpy(p, q, sizeof p);
    }
    size_t l = strlen(p) + 1;
    if (l > sz) return -ERANGE;
    if (copyout(t->mm, ubuf, p, l) < 0) return -EFAULT;
    return (long)l;
}

static long sys_truncate(struct node *n, int64_t len) { if (n->type != N_REG) return -EINVAL; return fs_truncate(n, (uint64_t)len); }

/* ── memory ─────────────────────────────────────────────────────────── */
static long sys_mmap(struct task *t, uint64_t addr, uint64_t len, long prot, long flags, long fd, uint64_t off)
{
    if (!len || (off & 0xFFF)) return -EINVAL;
    len = (len + 0xFFF) & ~0xFFFULL;
    struct node *file = 0;
    if (!(flags & 0x20)) {
        struct file *f = getf(t, fd);
        if (!f) return -EBADF;
        if (!f->node || (f->node->type != N_REG && !(f->node->type == N_CHR && f->node->dev == DEV_ZERO))) return -ENODEV;
        if (f->node->type == N_REG) file = f->node;
        if ((flags & 3) == 1 && (prot & 2) && file && ((f->flags & 3) == 0 || vfs_readonly(f->mnt))) return -EACCES;
    }
    int shared = (flags & 3) == 1;
    uint64_t at;
    if (flags & 0x10) {                                     /* MAP_FIXED */
        if (addr & 0xFFF) return -EINVAL;
        at = addr;
    } else {
        at = 0;
        if (addr && !(addr & 0xFFF) && addr + len <= USER_TOP) {
            int free = 1;
            for (int i = 0; i < t->mm->nv; i++) if (t->mm->v[i].start < addr + len && t->mm->v[i].end > addr) { free = 0; break; }
            if (free) at = addr;
        }
        if (!at) at = mm_find_free(t->mm, len, 0);
        if (!at) return -ENOMEM;
    }
    long r = mm_map(t->mm, at, len, (uint32_t)(prot & 7), shared, file, off, file ? ~0ULL / 2 : 0);
    return r < 0 ? r : (long)at;
}

static long sys_brk(struct task *t, uint64_t want)
{
    struct mm *m = t->mm;
    if (want < m->brk_start) return (long)m->brk;
    uint64_t old = (m->brk + 0xFFF) & ~0xFFFULL, nw = (want + 0xFFF) & ~0xFFFULL;
    if (nw > old) { if (mm_map(m, old, nw - old, VM_READ | VM_WRITE, 0, 0, 0, 0) < 0) return (long)m->brk; }
    else if (nw < old) mm_unmap(m, nw, old - nw);
    m->brk = want;
    return (long)want;
}

static long sys_mremap(struct task *t, uint64_t old, uint64_t olen, uint64_t nlen, long flags)
{
    olen = (olen + 0xFFF) & ~0xFFFULL; nlen = (nlen + 0xFFF) & ~0xFFFULL;
    struct vma *v = mm_vma_at(t->mm, old);
    if (!v || old & 0xFFF) return -EFAULT;
    if (nlen <= olen) { if (nlen < olen) mm_unmap(t->mm, old + nlen, olen - nlen); return (long)old; }
    /* grow in place if the space after is free */
    int free = 1;
    for (int i = 0; i < t->mm->nv; i++) if (t->mm->v[i].start < old + nlen && t->mm->v[i].end > old + olen) { free = 0; break; }
    struct vma copy = *v;
    if (free) {
        uint64_t foff = copy.off + (old - copy.start) + olen;
        if (mm_map(t->mm, old + olen, nlen - olen, copy.prot, (int)copy.shared, copy.file, foff, copy.flen > (old - copy.start) + olen ? copy.flen - (old - copy.start) - olen : 0) < 0) return -ENOMEM;
        return (long)old;
    }
    if (!(flags & 1)) return -ENOMEM;
    uint64_t at = mm_find_free(t->mm, nlen, 0);
    if (!at) return -ENOMEM;
    if (mm_map(t->mm, at, nlen, copy.prot, (int)copy.shared, copy.file, copy.off + (old - copy.start), copy.flen) < 0) return -ENOMEM;
    /* move the contents */
    uint8_t *tmp = rs_kmalloc(4096);
    if (!tmp) return -ENOMEM;
    for (uint64_t o = 0; o < olen; o += 4096) {
        if (copyin(t->mm, tmp, old + o, 4096) == 0) copyout(t->mm, at + o, tmp, 4096);
    }
    rs_kfree(tmp);
    mm_unmap(t->mm, old, olen);
    return (long)at;
}

/* ── processes ──────────────────────────────────────────────────────── */
struct child_start { struct task *t; struct trapframe tf; uint64_t fs; volatile int go; };

static void child_entry(uintptr_t arg)
{
    struct child_start *cs = (struct child_start *)arg;
    /* the parent writes the new id (CLONE_PARENT_SETTID / CHILD_SETTID) first, as Linux does
       before the child can run: a thread that ended at once would otherwise have its cleared
       id written back, and pthread_join would wait for ever */
    while (!cs->go) rs_yield();
    struct task *t = cs->t;
    struct trapframe tf = cs->tf;
    t->pid = rs_current_pid();
    if (!t->is_thread) t->tgid = t->pid;
    t->ns.lpid[0] = t->pid; t->ns.ltgid[0] = t->tgid;
    t->start_tick = kernel_ticks();
    rs_set_ucr3(t->mm->pml4);
    wrmsr(0xC0000100, cs->fs);
    rs_kfree(cs);
    user_return(&tf);
}

static long sys_clone(struct task *t, uint64_t flags, uint64_t stack, uint64_t ptid, uint64_t ctid, uint64_t tls)
{
    struct task *c = task_alloc();
    if (!c) return -EAGAIN;
    int vm = (flags & 0x100) != 0, vfork = (flags & 0x4000) != 0, thread = (flags & 0x10000) != 0;
    c->ppid = thread ? t->ppid : t->tgid;
    c->pgid = t->pgid;
    c->umask = t->umask;
    c->cwd = t->cwd;
    c->is_thread = thread;
    ns_task_init(c, t);
    long nr_ = ns_clone(t, c, flags);
    if (nr_ < 0) { ns_task_exit(c); task_free(c); return nr_; }
    ns_child_pid(t, c, 0, thread ? t->tgid : 0);
    c->tgid = thread ? t->tgid : 0;
    strlcpy(c->name, t->name, sizeof c->name);
    strlcpy(c->exe, t->exe, sizeof c->exe);
    c->sigmask = t->sigmask;
    if (vm) { c->mm = t->mm; t->mm->refs++; }
    else if (!(c->mm = mm_clone(t->mm))) { task_free(c); return -ENOMEM; }
    if (flags & 0x400) { c->files = t->files; t->files->refs++; }          /* CLONE_FILES */
    else {
        c->files = rs_kmalloc(sizeof *c->files);
        if (!c->files) { mm_put(c->mm); task_free(c); return -ENOMEM; }
        *c->files = *t->files;
        c->files->refs = 1;
        for (int i = 0; i < MAXFD; i++) if (c->files->fd[i]) c->files->fd[i]->refs++;
    }
    if (flags & 0x800) c->sig = t->sig;                                     /* CLONE_SIGHAND */
    else {
        c->sig = rs_kmalloc(sizeof(struct sigact) * 64);
        if (c->sig) memcpy(c->sig, t->sig, sizeof(struct sigact) * 64);
    }
    struct child_start *cs = rs_kmalloc(sizeof *cs);
    if (!cs) return -ENOMEM;
    cs->go = 0;
    cs->t = c;
    cs->tf = *t->tf;
    cs->tf.rax = 0;
    if (stack) cs->tf.rsp = stack;
    cs->fs = (flags & 0x80000) ? tls : rdmsr(0xC0000100);
    if (flags & 0x00200000) c->clear_child_tid = ctid;                     /* CLONE_CHILD_CLEARTID */
    int pid = rs_spawn(c->name, child_entry, (uintptr_t)cs);
    if (pid < 0) { rs_kfree(cs); task_free(c); return -EAGAIN; }
    c->pid = pid;
    if (!thread) c->tgid = pid;
    c->ns.lpid[0] = pid; c->ns.ltgid[0] = c->tgid;
    { struct pidns *pn = c->ns.pid; if (pn && pn->level > 0 && c->ns.lpid[pn->level] == 1) pn->init_tgid = c->tgid; }
    int vpid = pid_vpid(t, c);                                               /* as the parent numbers it */
    if (flags & 0x100000) { int p32 = vpid; copyout(t->mm, ptid, &p32, 4); }  /* CLONE_PARENT_SETTID */
    if (flags & 0x01000000) { int p32 = pid_vpid(c, c); copyout(c->mm, ctid, &p32, 4); }  /* CLONE_CHILD_SETTID */
    cs->go = 1;                                              /* (the child frees cs: not touched after this) */
    if (vfork) { while (!c->vfork_done && c->used && c->state != 2) wait_tick(); }
    return vpid;
}

static void close_files(struct task *t)
{
    if (!t->files) return;
    if (--t->files->refs == 0) {
        for (int i = 0; i < MAXFD; i++) if (t->files->fd[i]) file_put(t->files->fd[i]);
        rs_kfree(t->files);
    }
    t->files = 0;
}

static void futex_wake_addr(struct mm *m, uint64_t addr, int n);

void task_exit(struct task *t, int status)
{
    if (t->clear_child_tid && t->mm) {
        int z = 0;
        copyout(t->mm, t->clear_child_tid, &z, 4);
        futex_wake_addr(t->mm, t->clear_child_tid, 1);
    }
    t->vfork_done = 1;
    ns_task_exit(t);
    close_files(t);
    if (t->mm) { struct mm *m = t->mm; t->mm = 0; mm_put(m); }
    rs_set_ucr3(0);
    if (t->is_thread) { task_free(t); rs_thread_exit(); }
    /* children of this process go to the reaper: a subreaper ancestor or the PID namespace's
       init; in the first namespace they're nobody's */
    int reaper = pid_reaper(t);
    for (int i = 0; i < MAXTASK; i++) {
        struct task *c = task_slot(i);
        if (!c->used || c->ppid != t->tgid || c == t) continue;
        c->ppid = reaper;
        if (reaper && c->state == 2) { struct task *rp = task_by_pid(reaper); if (rp) rp->pending |= 1ULL << 16; }
    }
    t->exit_status = status;
    struct task *parent = t->ppid > 0 ? task_by_pid(t->ppid) : 0;
    if (parent) parent->pending |= 1ULL << 16;              /* SIGCHLD */
    t->state = 2;                                           /* zombie: the parent's wait4 reaps it */
    /* nobody will wait for orphans: reap now */
    if (t->detached || (t->ppid && !task_by_pid(t->ppid))) task_free(t);
    rs_thread_exit();
}

static void exit_group(struct task *t, int status)
{
    for (int i = 0; i < MAXTASK; i++) {
        struct task *o = task_slot(i);
        if (o->used && o != t && o->tgid == t->tgid && o->state != 2) o->pending |= 1ULL << 8;   /* SIGKILL */
    }
    task_exit(t, status);
}

static long sys_wait4(struct task *t, long pid, uint64_t ustatus, long options)
{
    if (pid > 0) { pid = pid_global(t, (int)pid, 1); if (!pid) return -ECHILD; }
    else if (pid < -1) { int g = pid_global(t, (int)-pid, 1); if (!g) return -ECHILD; pid = -g; }
    for (;;) {
        int have = 0;
        for (int i = 0; i < MAXTASK; i++) {
            struct task *c = task_slot(i);
            if (!c->used || c->is_thread || c->ppid != t->tgid) continue;
            if (pid > 0 && c->pid != pid) continue;
            if (pid == 0 && c->pgid != t->pgid) continue;
            if (pid < -1 && c->pgid != -pid) continue;
            have = 1;
            if (c->state == 2) {
                int st = c->exit_status, cp = pid_vtgid(t, c);
                if (ustatus && copyout(t->mm, ustatus, &st, 4) < 0) return -EFAULT;
                task_free(c);
                return cp;
            }
        }
        if (!have) return -ECHILD;
        if (options & 1) return 0;                            /* WNOHANG */
        if (t->pending & ~t->sigmask & ~(1ULL << 16)) return -EINTR;
        wait_tick();
    }
}

static long sys_execve(struct task *t, uint64_t upath_, uint64_t uargv, uint64_t uenvp)
{
    char path[1024];
    long r;
    if ((r = upath(t, upath_, path, sizeof path)) < 0) return r;
    /* gather argv / envp into kernel memory */
    char *space = rs_kmalloc(256 * 1024);
    char **vec = rs_kmalloc(sizeof(char *) * 4100);
    if (!space || !vec) { if (space) rs_kfree(space); if (vec) rs_kfree(vec); return -ENOMEM; }
    size_t used = 0;
    int na = 0, ne = 0;
    char **argv = vec, **envp;
    for (int pass = 0; pass < 2; pass++) {
        uint64_t base = pass ? uenvp : uargv;
        char **out = pass ? vec + na + 1 : vec;
        int k = 0;
        for (; base; k++) {
            uint64_t p;
            if (k >= 2048 || copyin(t->mm, &p, base + (uint64_t)k * 8, 8) < 0) { r = -E2BIG; goto out; }
            if (!p) break;
            long l = copyin_str(t->mm, space + used, p, 256 * 1024 - used);
            if (l < 0) { r = l == -36 ? -E2BIG : l; goto out; }
            out[k] = space + used;
            used += (size_t)l + 1;
        }
        out[k] = 0;
        if (pass) ne = k; else na = k;
    }
    envp = vec + na + 1;
    (void)ne;
    if (!na) { argv[0] = path; argv[1] = 0; }
    r = exec_load(t, path, argv, envp, t->tf);
    if (r == 0) {
        for (int i = 0; i < MAXFD; i++)
            if (t->files->cloexec[i] && t->files->fd[i]) { file_put(t->files->fd[i]); t->files->fd[i] = 0; t->files->cloexec[i] = 0; }
        for (int i = 0; i < 64; i++) if (t->sig[i].handler > 1) t->sig[i].handler = 0;   /* handlers reset; ignores stay */
        t->vfork_done = 1;
    }
out:
    rs_kfree(space);
    rs_kfree(vec);
    return r;
}

/* ── futexes ────────────────────────────────────────────────────────── */
#define NFUTEX 512
static struct { struct mm *mm; uint64_t addr; int pid; volatile int woken; } fw[NFUTEX];

static void futex_wake_addr(struct mm *m, uint64_t addr, int n)
{
    for (int i = 0; i < NFUTEX && n > 0; i++)
        if (fw[i].pid && fw[i].mm == m && fw[i].addr == addr && !fw[i].woken) { fw[i].woken = 1; n--; }
}

static uint32_t boot_epoch;
static void now_ts(int clk, int64_t *s, int64_t *ns);
static long sys_futex(struct task *t, uint64_t addr, long op, uint64_t val, uint64_t utimeout)
{
    int cmd = (int)(op & 127);
    if (cmd == 1 || cmd == 10) {                          /* WAKE, WAKE_BITSET */
        int n = 0;
        for (int i = 0; i < NFUTEX; i++)
            if (fw[i].pid && fw[i].mm == t->mm && fw[i].addr == addr && !fw[i].woken && n < (int)val) { fw[i].woken = 1; n++; }
        return n;
    }
    if (cmd == 0 || cmd == 9) {                           /* WAIT, WAIT_BITSET */
        uint32_t cur;
        uint64_t until = ~0ULL;
        if (utimeout) {
            int64_t ts[2];
            if (copyin(t->mm, ts, utimeout, 16) < 0) return -EFAULT;
            if (cmd == 9 && (op & 256)) {                     /* FUTEX_CLOCK_REALTIME: wall-clock time */
                if (!boot_epoch) { int64_t a_, b_; now_ts(0, &a_, &b_); }
                ts[0] -= (int64_t)boot_epoch;
                if (ts[0] < 0) ts[0] = 0;
            }
            uint64_t ticks = (uint64_t)ts[0] * AUXO_HZ + (uint64_t)ts[1] / (1000000000 / AUXO_HZ);
            until = cmd == 9 ? ticks : kernel_ticks() + ticks;  /* BITSET timeouts are absolute (CLOCK_MONOTONIC) */
        }
        int slot = -1;
        uint64_t f;
        __asm__ volatile("pushfq; pop %0; cli" : "=r"(f) :: "memory");
        for (int i = 0; i < NFUTEX; i++) if (!fw[i].pid) { slot = i; fw[i].pid = t->pid; fw[i].mm = t->mm; fw[i].addr = addr; fw[i].woken = 0; break; }
        if (f & 0x200) __asm__ volatile("sti");
        if (slot < 0) { wait_tick(); return 0; }
        /* the value is checked only once we're on the list: a wake between the check and the
           sleep (a thread exiting just as pthread_join looks) is then never lost */
        if (copyin(t->mm, &cur, addr, 4) < 0) { fw[slot].pid = 0; return -EFAULT; }
        if (cur != (uint32_t)val) { fw[slot].pid = 0; return -EAGAIN; }
        long r = 0;
        while (!fw[slot].woken) {
            if (kernel_ticks() >= until) { r = -110; break; }      /* ETIMEDOUT */
            if (t->pending & ~t->sigmask) { r = -EINTR; break; }
            wait_tick();
        }
        fw[slot].pid = 0;
        return r;
    }
    if (cmd == 5 || cmd == 4 || cmd == 3) return 0;       /* WAKE_OP, CMP_REQUEUE, REQUEUE: treat as no-ops */
    return -ENOSYS;
}

/* ── poll / select ──────────────────────────────────────────────────── */
static int fd_ready(struct task *t, int fd, int events)
{
    struct file *f = getf(t, fd);
    if (!f) return 0x20;                                  /* POLLNVAL */
    int r = 0;
    if (f->kind == 8) r = inet_poll(f->priv);
    else if (f->kind == 5) { if (unix_readable(f->priv)) r |= 1; if (unix_writable(f->priv)) r |= 4; if (((struct usock *)f->priv)->peer_closed) r |= 0x10; }
    else if (f->kind == 6) { if (*(uint64_t *)f->priv) r |= 1; r |= 4; }
    else if (f->kind == 9) { if (t->pending & *(uint64_t *)f->priv) r |= 1; }
    else if (f->kind == 10) { if (nl_readable(f->priv)) r |= 1; r |= 4; }
    else if (f->kind == 7) {                                   /* an epoll set is readable when a member is */
        struct { int n; struct { int fd; uint32_t events; uint64_t data; } e[64]; } *ep = f->priv;
        for (int i = 0; i < ep->n; i++) {
            struct file *m = getf(t, ep->e[i].fd);
            if (m && m->kind != 7 && (ep->e[i].events & 0xFF) && fd_ready(t, ep->e[i].fd, (int)(ep->e[i].events & 0xFF))) { r |= 1; break; }
        }
    }
    else if (f->kind == 1) { if (pipe_readable(f)) r |= 1; }
    else if (f->kind == 2) { if (pipe_writable(f)) r |= 4; }
    else if (f->node && f->node->type == N_CHR && f->node->dev == DEV_TTY) { if (tty_readable()) r |= 1; r |= 4; }
    else r |= 5;
    return r & (events | 0x18);
}

static long sys_poll(struct task *t, uint64_t ufds, long n, int64_t timeout_ms)
{
    if (n > 256) return -EINVAL;
    uint64_t until = timeout_ms < 0 ? ~0ULL : kernel_ticks() + (uint64_t)(timeout_ms + 9) / 10;
    struct { int fd; short ev, rev; } p[256];
    if (copyin(t->mm, p, ufds, (size_t)n * 8) < 0) return -EFAULT;
    for (;;) {
        int cnt = 0;
        for (long i = 0; i < n; i++) {
            p[i].rev = p[i].fd < 0 ? 0 : (short)fd_ready(t, p[i].fd, p[i].ev);
            if (p[i].rev) cnt++;
        }
        if (cnt || kernel_ticks() >= until) {
            if (copyout(t->mm, ufds, p, (size_t)n * 8) < 0) return -EFAULT;
            return cnt;
        }
        if (t->pending & ~t->sigmask) return -EINTR;
        wait_tick();
    }
}

static long sys_select(struct task *t, long n, uint64_t ur, uint64_t uw, uint64_t ue, int64_t timeout_ms)
{
    uint64_t r[16] = { 0 }, w[16] = { 0 }, rr[16], ww[16];
    if (n > 1024) n = 1024;
    size_t bytes = (size_t)((n + 63) / 64) * 8;
    if (ur && copyin(t->mm, r, ur, bytes) < 0) return -EFAULT;
    if (uw && copyin(t->mm, w, uw, bytes) < 0) return -EFAULT;
    uint64_t until = timeout_ms < 0 ? ~0ULL : kernel_ticks() + (uint64_t)(timeout_ms + 9) / 10;
    for (;;) {
        int cnt = 0;
        memset(rr, 0, sizeof rr); memset(ww, 0, sizeof ww);
        for (int fd = 0; fd < n; fd++) {
            uint64_t bit = 1ULL << (fd % 64);
            if ((r[fd / 64] & bit) && (fd_ready(t, fd, 1) & 1)) { rr[fd / 64] |= bit; cnt++; }
            if ((w[fd / 64] & bit) && (fd_ready(t, fd, 4) & 4)) { ww[fd / 64] |= bit; cnt++; }
        }
        if (cnt || kernel_ticks() >= until) {
            if (ur) copyout(t->mm, ur, rr, bytes);
            if (uw) copyout(t->mm, uw, ww, bytes);
            if (ue) { uint64_t z[16] = { 0 }; copyout(t->mm, ue, z, bytes); }
            return cnt;
        }
        if (t->pending & ~t->sigmask) return -EINTR;
        wait_tick();
    }
}

/* ── time ───────────────────────────────────────────────────────────── */

static void now_ts(int clk, int64_t *s, int64_t *ns)
{
    uint64_t tk = kernel_ticks();
    if (!boot_epoch) boot_epoch = fs_now() - (uint32_t)(tk / AUXO_HZ);
    /* ticks are 10 ms; mix in the TSC so successive calls differ */
    uint64_t sub = (rdtsc() >> 10) % (1000000000 / AUXO_HZ);
    if (clk == 0 || clk == 5 || clk == 8) *s = (int64_t)boot_epoch + (int64_t)(tk / AUXO_HZ);
    else *s = (int64_t)(tk / AUXO_HZ);
    *ns = (int64_t)((tk % AUXO_HZ) * (1000000000 / AUXO_HZ) + sub);
}

static long sys_nanosleep(struct task *t, uint64_t ureq)
{
    int64_t ts[2];
    if (copyin(t->mm, ts, ureq, 16) < 0) return -EFAULT;
    uint64_t ticks = (uint64_t)ts[0] * AUXO_HZ + (uint64_t)(ts[1] + 9999999) / 10000000;
    uint64_t until = kernel_ticks() + ticks;
    while (kernel_ticks() < until) { if (t->pending & ~t->sigmask) return -EINTR; rs_sleep(until - kernel_ticks()); }
    return 0;
}

/* ── signals ────────────────────────────────────────────────────────── */
static int default_ignored(int sig) { return sig == 17 || sig == 28 || sig == 23 || sig == 18; }   /* CHLD WINCH URG CONT */

/* a program died of a crash: where it was, and the return addresses on its stack that point
 * into code, as file+offset (for addr2line / nm on the same files) */
static void where(struct task *t, uint64_t a, char *out, size_t n)
{
    struct vma *v = mm_vma_at(t->mm, a);
    if (!v || !(v->prot & 4)) { out[0] = 0; return; }
    char fp[96] = "?";
    if (v->file) { const char *nm = v->file->name; strlcpy(fp, nm ? nm : "?", sizeof fp); }
    ksnprintf(out, n, "%s+%lx", fp, a - v->start + v->off);
}

static void crash_report(struct task *t, struct trapframe *tf, int sig)
{
    char w[128];
    where(t, tf->rip, w, sizeof w);
    kprintf("[%s (pid %d) died of signal %d at %s]\n", t->name, t->pid, sig, w[0] ? w : "?");
    uint64_t fp = tf->rbp;                                    /* the frame-pointer chain, where there is one */
    for (int i = 0; i < 16 && fp; i++) {
        uint64_t fr[4];
        if (copyin(t->mm, fr, fp, 32) < 0) break;
        where(t, fr[1], w, sizeof w);
        kprintf("[  fp %s  (%lx %lx)]\n", w[0] ? w : "?", fr[2], fr[3]);
        if (fr[0] <= fp) break;
        fp = fr[0];
    }
    int shown = 0;
    for (int i = 0; i < 512 && shown < 12; i++) {
        uint64_t v;
        if (copyin(t->mm, &v, tf->rsp + (uint64_t)i * 8, 8) < 0) break;
        where(t, v, w, sizeof w);
        if (w[0]) { kprintf("[  %s]\n", w); shown++; }
    }
}

/* auxsh `ustack PID`: for each thread of a process, the system call it's in and its call chain */
void uproc_where(int pid)
{
    for (int i = 0; i < MAXTASK; i++) {
        struct task *o = task_slot(i);
        if (!o->used || o->tgid != pid || !o->tf || !o->mm) continue;
        char w[128];
        where(o, o->tf->rip, w, sizeof w);
        kprintf("thread %d %s: syscall %lu (%lx, %lx, %lx) at %s\n", o->pid, o->name, (unsigned long)o->tf->rax, o->tf->rdi, o->tf->rsi, o->tf->rdx, w[0] ? w : "?");
        uint64_t fp = o->tf->rbp;
        for (int k = 0; k < 14 && fp; k++) {
            uint64_t fr[2];
            if (copyin(o->mm, fr, fp, 16) < 0) break;
            where(o, fr[1], w, sizeof w);
            if (w[0]) kprintf("    %s\n", w);
            if (fr[0] <= fp) break;
            fp = fr[0];
        }
    }
}

/* called on every return to user mode: deliver what's pending (handlers, or the default action) */
void uproc_signals(struct task *t, struct trapframe *tf)
{
    uint64_t deliverable = t->pending & ~t->sigmask;
    if (t->pending & (1ULL << 8)) deliverable |= 1ULL << 8;   /* SIGKILL can't be blocked */
    if (!deliverable) return;
    int sig = __builtin_ctzll(deliverable) + 1;
    t->pending &= ~(1ULL << (sig - 1));
    struct sigact *a = &t->sig[sig - 1];
    if (sig == 9 || a->handler == 0) {
        if (default_ignored(sig)) return;
        if (sig == 11) kprintf("Segmentation fault\n");
        else if (sig == 4) kprintf("Illegal instruction\n");
        else if (sig == 8) kprintf("Floating point exception\n");
        else if (sig == 6) kprintf("Aborted\n");
        if (sig == 4 || sig == 5 || sig == 6 || sig == 7 || sig == 8 || sig == 11) crash_report(t, tf, sig);
        exit_group(t, sig);
    }
    if (a->handler == 1) return;                              /* SIG_IGN */
    /* build an rt_sigframe: return address (sa_restorer), ucontext, siginfo */
    uint64_t sp = (tf->rsp - 128 - 1024) & ~15ULL;            /* below the red zone */
    uint8_t uc[944];
    memset(uc, 0, sizeof uc);
    /* ucontext: flags, link, stack (24 bytes), then mcontext gregs at offset 40 */
    uint64_t *g = (uint64_t *)(uc + 40);
    g[0] = tf->r8; g[1] = tf->r9; g[2] = tf->r10; g[3] = tf->r11; g[4] = tf->r12; g[5] = tf->r13; g[6] = tf->r14; g[7] = tf->r15;
    g[8] = tf->rdi; g[9] = tf->rsi; g[10] = tf->rbp; g[11] = tf->rbx; g[12] = tf->rdx; g[13] = tf->rax; g[14] = tf->rcx;
    g[15] = tf->rsp; g[16] = tf->rip; g[17] = tf->rflags; g[18] = 0x23 | (0x1bULL << 48);
    memcpy(uc + 296, &t->sigmask, 8);                         /* uc_sigmask */
    uint8_t info[128];
    memset(info, 0, sizeof info);
    int s32 = sig;
    memcpy(info, &s32, 4);
    sp -= sizeof info;
    uint64_t uinfo = sp;
    sp -= sizeof uc;
    uint64_t uuc = sp;
    sp -= 8;
    if (copyout(t->mm, uinfo, info, sizeof info) < 0 || copyout(t->mm, uuc, uc, sizeof uc) < 0 ||
        copyout(t->mm, sp, &a->restorer, 8) < 0) { exit_group(t, 11); }
    if (!(a->flags & 0x40000000)) t->sigmask |= a->mask | (1ULL << (sig - 1));   /* unless SA_NODEFER */
    tf->rsp = sp;
    tf->rip = a->handler;
    tf->rdi = (uint64_t)sig;
    tf->rsi = uinfo;
    tf->rdx = uuc;
    tf->rax = 0;
    if (a->flags & 0x80000000) a->handler = 0;                /* SA_RESETHAND */
}

static long sys_rt_sigreturn(struct task *t, struct trapframe *tf)
{
    uint8_t uc[944];
    uint64_t uuc = tf->rsp;                                   /* after the handler's ret popped the restorer */
    if (copyin(t->mm, uc, uuc, sizeof uc) < 0) exit_group(t, 11);
    uint64_t *g = (uint64_t *)(uc + 40);
    tf->r8 = g[0]; tf->r9 = g[1]; tf->r10 = g[2]; tf->r11 = g[3]; tf->r12 = g[4]; tf->r13 = g[5]; tf->r14 = g[6]; tf->r15 = g[7];
    tf->rdi = g[8]; tf->rsi = g[9]; tf->rbp = g[10]; tf->rbx = g[11]; tf->rdx = g[12]; tf->rax = g[13]; tf->rcx = g[14];
    tf->rsp = g[15]; tf->rip = g[16]; tf->rflags = (g[17] & 0xCD5) | 0x202;
    memcpy(&t->sigmask, uc + 296, 8);
    return (long)tf->rax;
}

/* a namespace's init only takes signals from inside that it has a handler for */
static int init_protected(struct task *t, struct task *o, long sig)
{
    struct pidns *ns = o->ns.pid;
    if (!ns || ns->level == 0 || ns->init_tgid != o->tgid || t->ns.pid != ns) return 0;
    return !o->sig || o->sig[sig - 1].handler == 0 || o->sig[sig - 1].handler == 1 || sig == 9 || sig == 19;
}

static long sys_kill(struct task *t, long pid, long sig)
{
    if (sig < 0 || sig > 64) return -EINVAL;
    if (pid > 0) { pid = pid_global(t, (int)pid, 1); if (!pid) return -ESRCH; }
    else if (pid < -1) { int g = pid_global(t, (int)-pid, 1); if (!g) return -ESRCH; pid = -g; }
    int found = 0;
    for (int i = 0; i < MAXTASK; i++) {
        struct task *o = task_slot(i);
        if (!o->used || o->state == 2 || !pid_visible(t, o)) continue;
        int match = pid > 0 ? o->tgid == pid && !o->is_thread : pid == 0 ? o->pgid == t->pgid : pid == -1 ? o != t : o->pgid == -pid;
        if (!match) continue;
        found = 1;
        if (sig && !init_protected(t, o, sig)) o->pending |= 1ULL << (sig - 1);
    }
    return found ? 0 : -ESRCH;
}

/* tkill / tgkill: one thread */
static long sys_tkill(struct task *t, long tgid, long tid, long sig)
{
    if (sig < 0 || sig > 64) return -EINVAL;
    int g = pid_global(t, (int)tid, 0);
    struct task *o = g ? task_by_pid(g) : 0;
    if (!o || o->state == 2 || (tgid > 0 && pid_vtgid(t, o) != tgid)) return -ESRCH;
    if (sig && !init_protected(t, o, sig)) o->pending |= 1ULL << (sig - 1);
    return 0;
}

/* ── misc ───────────────────────────────────────────────────────────── */
static long sys_uname(struct task *t, uint64_t u)
{
    char b[390];
    memset(b, 0, sizeof b);
    strlcpy(b, "Linux", 65);
    strlcpy(b + 65, uts_host(t), 65);
    strlcpy(b + 130, "6.8.0-auxobsd", 65);
    strlcpy(b + 195, "#1 AuxoBSD " AUXO_VERSION, 65);
    strlcpy(b + 260, "x86_64", 65);
    strlcpy(b + 325, t->ns.uts && !t->ns.uts->init ? t->ns.uts->domain : "auxolinux.com", 65);
    return copyout(t->mm, u, b, sizeof b);
}

static long sys_ioctl(struct task *t, long fd, uint64_t req, uint64_t arg)
{
    struct file *f = getf(t, fd);
    if (!f) return -EBADF;
    int tty = f->node && f->node->type == N_CHR && f->node->dev == DEV_TTY;
    if (req == 0x5421) { if (arg) { int v; if (copyin(t->mm, &v, arg, 4) == 0) f->flags = v ? f->flags | 04000 : f->flags & ~04000; } return 0; }
    if ((f->kind == 5 || f->kind == 8 || f->kind == 10) && (req == 0x8933 || req == 0x8913)) {   /* SIOCGIFINDEX, SIOCGIFFLAGS */
        uint8_t ifr[40];
        if (copyin(t->mm, ifr, arg, 40) < 0) return -EFAULT;
        ifr[15] = 0;
        int idx = !strcmp((char *)ifr, "lo") ? 1 : (!net_isolated(t) && !strcmp((char *)ifr, "eth0")) ? 2 : 0;
        if (!idx) return -ENODEV;
        if (req == 0x8933) memcpy(ifr + 16, &idx, 4);
        else { short fl = idx == 1 ? (short)(0x8 | ((!net_isolated(t) || t->ns.net->lo_up) ? 0x41 : 0)) : 0x1043; memcpy(ifr + 16, &fl, 2); }
        return copyout(t->mm, arg, ifr, 40);
    }
    if (!tty) return -ENOTTY;
    switch (req) {
    case 0x5401: {                                           /* TCGETS */
        uint8_t tio[60];
        memset(tio, 0, sizeof tio);
        uint32_t iflag = 0x500, oflag = 0x5, cflag = 0xBF, lflag = (tty_icanon ? 0x2 : 0) | (tty_echo ? 0x8 | 0x10 | 0x20 | 0x200 : 0) | 0x1 | 0x8000;
        memcpy(tio, &iflag, 4); memcpy(tio + 4, &oflag, 4); memcpy(tio + 8, &cflag, 4); memcpy(tio + 12, &lflag, 4);
        static const uint8_t cc[] = { 3, 28, 127, 21, 4, 0, 1, 0, 17, 19, 26, 0, 18, 15, 23, 22 };
        memcpy(tio + 17, cc, sizeof cc);
        return copyout(t->mm, arg, tio, 36);
    }
    case 0x5402: case 0x5403: case 0x5404: {               /* TCSETS* */
        uint32_t lflag;
        if (copyin(t->mm, &lflag, arg + 12, 4) < 0) return -EFAULT;
        tty_icanon = (lflag & 0x2) != 0;
        tty_echo = (lflag & 0x8) != 0;
        return 0;
    }
    case 0x5413: {                                           /* TIOCGWINSZ */
        int w, h, cx, cy; uint32_t gen;
        console_grid_info(&w, &h, &cx, &cy, &gen);
        uint16_t ws[4] = { (uint16_t)h, (uint16_t)w, 0, 0 };
        return copyout(t->mm, arg, ws, 8);
    }
    case 0x540F: { int p = t->pgid; return copyout(t->mm, arg, &p, 4); }   /* TIOCGPGRP */
    case 0x5410: { int p; if (copyin(t->mm, &p, arg, 4) == 0) uproc_set_fg(p); return 0; }
    case 0x541B: { int n = tty_readable(); return copyout(t->mm, arg, &n, 4); }
    case 0x5414: case 0x540E: case 0x5422: case 0x5429: return 0;
    }
    return -EINVAL;
}

static long sys_fcntl(struct task *t, long fd, long cmd, uint64_t arg)
{
    struct file *f = getf(t, fd);
    if (!f) return -EBADF;
    switch (cmd) {
    case 0: case 1030: { int n = alloc_fd(t, f, (int)arg, cmd == 1030); if (n >= 0) f->refs++; return n; }
    case 1: return t->files->cloexec[fd];
    case 2: t->files->cloexec[fd] = (uint8_t)(arg & 1); return 0;
    case 3: return f->flags & ~02000000;
    case 4: f->flags = (f->flags & 3) | (int)(arg & (02000 | 04000)); return 0;
    case 5: case 36: {                                     /* F_GETLK, F_OFD_GETLK: nobody holds a conflicting lock */
        short unl = 2;                                     /* F_UNLCK (SQLite reads this to see the database is free) */
        return copyout(t->mm, arg, &unl, 2);
    }
    case 6: case 7: case 37: case 38: return 0;           /* set locks: always granted */
    case 1031: case 1033: case 1034: return 0;
    }
    return -EINVAL;
}

static long sys_dup3(struct task *t, long old, long nw, long flags)
{
    struct file *f = getf(t, old);
    if (!f || nw < 0 || nw >= MAXFD) return -EBADF;
    if (old == nw) return nw;
    if (t->files->fd[nw]) file_put(t->files->fd[nw]);
    t->files->fd[nw] = f;
    t->files->cloexec[nw] = (flags & 02000000) != 0;
    f->refs++;
    return nw;
}

static long sys_pipe2(struct task *t, uint64_t ufd, long flags)
{
    struct file *r, *w;
    if (pipe_new(&r, &w) < 0) return -ENOMEM;
    r->flags |= (int)(flags & 04000); w->flags |= (int)(flags & 04000);
    int a = alloc_fd(t, r, 0, (flags & 02000000) != 0);
    if (a < 0) { file_put(r); file_put(w); return a; }
    int b = alloc_fd(t, w, 0, (flags & 02000000) != 0);
    if (b < 0) { t->files->fd[a] = 0; file_put(r); file_put(w); return b; }
    int fds[2] = { a, b };
    return copyout(t->mm, ufd, fds, 8);
}

static long sys_memfd(struct task *t, uint64_t uname, long flags)
{
    char nm[64];
    if (copyin_str(t->mm, nm, uname, sizeof nm) < 0) strlcpy(nm, "memfd", sizeof nm);
    struct node *n = rs_kmalloc(sizeof *n);
    if (!n) return -ENOMEM;
    memset(n, 0, sizeof *n);
    n->name = rs_kmalloc(8);
    if (n->name) strlcpy(n->name, "memfd", 8);
    n->type = N_REG; n->mode = 0600; n->unlinked = 1; n->nlink = 0;
    struct file *f = file_new(0, n, 2);
    if (!f) return -ENOMEM;
    n->refs = 1;
    ksnprintf(f->path, sizeof f->path, "/memfd:%s (deleted)", nm);
    int fd = alloc_fd(t, f, 0, (flags & 1) != 0);
    if (fd < 0) file_put(f);
    return fd;
}

static long sys_sysinfo(struct task *t, uint64_t u)
{
    uint64_t b[14];
    memset(b, 0, sizeof b);
    b[0] = kernel_ticks() / AUXO_HZ;
    b[4] = pmm_total_pages() * PAGE_SIZE;
    b[5] = pmm_free_pages() * PAGE_SIZE;
    ((uint16_t *)b)[40] = (uint16_t)uproc_count();
    ((uint32_t *)b)[26] = 1;                                  /* mem_unit */
    return copyout(t->mm, u, b, 112);
}

static long sys_getrandom(struct task *t, uint64_t u, uint64_t n)
{
    uint8_t buf[256];
    if (n > sizeof buf) n = sizeof buf;
    uint64_t x = rdtsc() ^ 0x9E3779B97F4A7C15ULL;
    for (uint64_t i = 0; i < n; i++) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; x += rdtsc(); buf[i] = (uint8_t)(x >> 24); }
    return copyout(t->mm, u, buf, n) < 0 ? -EFAULT : (long)n;
}

static long sys_prlimit(struct task *t, long res, uint64_t unew, uint64_t uold)
{
    (void)unew;
    uint64_t lim[2] = { ~0ULL, ~0ULL };
    if (res == 3) { lim[0] = 8ULL << 20; }                   /* RLIMIT_STACK */
    if (res == 7) { lim[0] = lim[1] = MAXFD; }               /* RLIMIT_NOFILE */
    if (uold && copyout(t->mm, uold, lim, 16) < 0) return -EFAULT;
    return 0;
}


/* a network namespace of its own has only lo, and nothing listens on it */
static long isolated_dest(struct task *t, uint32_t ip)
{
    (void)t;
    return (ip >> 24) == 127 ? -ECONNREFUSED : -ENETUNREACH;
}

/* ── sockets (AF_INET): TCP clients and UDP, through kern/inet.c ─────── */
static struct isock *getisock(struct task *t, long fd)
{
    struct file *f = getf(t, fd);
    return f && f->kind == 8 ? f->priv : 0;
}

static long sin_in(struct task *t, uint64_t ua, uint64_t len, uint32_t *ip, uint16_t *port, int *fam)
{
    uint8_t sa[16];
    memset(sa, 0, sizeof sa);
    if (len < 2) return -EINVAL;
    if (copyin(t->mm, sa, ua, len < 16 ? len : 16) < 0) return -EFAULT;
    *fam = sa[0] | sa[1] << 8;
    if (*fam != 2 && *fam != 0) return -EAFNOSUPPORT;
    *port = (uint16_t)(sa[2] << 8 | sa[3]);
    *ip = (uint32_t)sa[4] << 24 | (uint32_t)sa[5] << 16 | (uint32_t)sa[6] << 8 | sa[7];
    return 0;
}

static long sin_out(struct task *t, uint64_t ua, uint64_t ulen, uint32_t ip, uint16_t port)
{
    if (!ua || !ulen) return 0;
    uint32_t l;
    if (copyin(t->mm, &l, ulen, 4) < 0) return -EFAULT;
    uint8_t sa[16] = { 2, 0, (uint8_t)(port >> 8), (uint8_t)port, (uint8_t)(ip >> 24), (uint8_t)(ip >> 16), (uint8_t)(ip >> 8), (uint8_t)ip };
    if (copyout(t->mm, ua, sa, l < 16 ? l : 16) < 0) return -EFAULT;
    l = 16;
    return copyout(t->mm, ulen, &l, 4);
}

static long inet_socket(struct task *t, long type, long proto)
{
    int st = (int)(type & 0xF);
    if (st != 1 && st != 2) return -93;                       /* EPROTONOSUPPORT: no raw sockets */
    if (proto && proto != (st == 1 ? 6 : 17)) return -93;
    struct isock *s = inet_new(st == 1 ? INET_STREAM : INET_DGRAM);
    if (!s) return -ENOMEM;
    struct file *f = file_new(8, 0, 2 | (int)(type & 04000));
    if (!f) { inet_close(s); return -ENOMEM; }
    f->priv = s;
    strlcpy(f->path, st == 1 ? "socket:[tcp]" : "socket:[udp]", sizeof f->path);
    int fd = alloc_fd(t, f, 0, (type & 02000000) != 0);
    if (fd < 0) file_put(f);
    return fd;
}

/* one message out of an inet socket: buffer already in the kernel */
static long inet_sendbuf(struct task *t, long fd, const uint8_t *kb, size_t n, uint64_t ua, uint64_t alen, long flags)
{
    struct isock *s = getisock(t, fd);
    uint32_t ip = 0; uint16_t port = 0; int fam = 2, to = 0;
    if (ua && alen && s->type == INET_DGRAM) { long r = sin_in(t, ua, alen, &ip, &port, &fam); if (r < 0) return r; to = 1; }
    if (net_isolated(t)) return isolated_dest(t, to ? ip : 0);
    long r = inet_send(s, kb, n, to, ip, port);
    if (r == -EPIPE && !(flags & 0x4000)) t->pending |= 1ULL << 12;   /* SIGPIPE unless MSG_NOSIGNAL */
    return r;
}

static long inet_sendto(struct task *t, long fd, uint64_t ubuf, uint64_t n, long flags, uint64_t ua, uint64_t alen)
{
    uint8_t *kb = kbuf_get();
    if (!kb) return -ENOMEM;
    long total = 0;
    do {
        size_t c = n > KBUF ? KBUF : n;
        if (copyin(t->mm, kb, ubuf, c) < 0) { total = total ? total : -EFAULT; break; }
        long r = inet_sendbuf(t, fd, kb, c, ua, alen, flags);
        if (r < 0) { total = total ? total : r; break; }
        total += r; ubuf += (uint64_t)r; n -= (uint64_t)r;
    } while (n && getisock(t, fd)->type == INET_STREAM);
    rs_kfree(kb);
    return total;
}

static long inet_recvfrom(struct task *t, long fd, uint64_t ubuf, uint64_t n, long flags, uint64_t ua, uint64_t alen)
{
    struct file *f = getf(t, fd);
    uint8_t *kb = kbuf_get();
    if (!kb) return -ENOMEM;
    uint32_t ip = 0; uint16_t port = 0;
    long r = inet_recv(t, f->priv, kb, n < KBUF ? n : KBUF, (f->flags & 04000) || (flags & 0x40), &ip, &port);
    if (r > 0 && copyout(t->mm, ubuf, kb, (size_t)r) < 0) r = -EFAULT;
    rs_kfree(kb);
    if (r >= 0 && ua) sin_out(t, ua, alen, ip, port);
    return r;
}

static long inet_msg(struct task *t, long fd, uint64_t umsg, long flags, int out)
{
    struct file *f = getf(t, fd);
    uint64_t m[7];
    if (copyin(t->mm, m, umsg, 56) < 0) return -EFAULT;
    uint8_t *kb = kbuf_get();
    if (!kb) return -ENOMEM;
    uint64_t iov[64][2], niov = m[3] < 64 ? m[3] : 64;
    size_t total = 0;
    for (uint64_t i = 0; i < niov; i++) { if (copyin(t->mm, iov[i], m[2] + i * 16, 16) < 0) { rs_kfree(kb); return -EFAULT; } }
    long r;
    if (out) {
        for (uint64_t i = 0; i < niov; i++) {
            size_t c = iov[i][1] < KBUF - total ? iov[i][1] : KBUF - total;
            if (c && copyin(t->mm, kb + total, iov[i][0], c) < 0) { rs_kfree(kb); return -EFAULT; }
            total += c;
        }
        r = inet_sendbuf(t, fd, kb, total, m[0], m[1], flags);
    } else {
        for (uint64_t i = 0; i < niov; i++) total += iov[i][1];
        uint32_t ip = 0; uint16_t port = 0;
        r = inet_recv(t, f->priv, kb, total < KBUF ? total : KBUF, (f->flags & 04000) || (flags & 0x40), &ip, &port);
        size_t o = 0;
        for (uint64_t i = 0; r > 0 && i < niov && o < (size_t)r; i++) {
            size_t c = (size_t)r - o < iov[i][1] ? (size_t)r - o : iov[i][1];
            if (copyout(t->mm, iov[i][0], kb + o, c) < 0) { r = -EFAULT; break; }
            o += c;
        }
        if (r >= 0) {
            if (m[0] && m[1] >= 16) {
                uint8_t sa[16] = { 2, 0, (uint8_t)(port >> 8), (uint8_t)port, (uint8_t)(ip >> 24), (uint8_t)(ip >> 16), (uint8_t)(ip >> 8), (uint8_t)ip };
                copyout(t->mm, m[0], sa, 16);
                uint32_t l = 16; copyout(t->mm, umsg + 8, &l, 4);
            }
            uint64_t zero = 0; int mf = 0;
            copyout(t->mm, umsg + 40, &zero, 8);              /* no control data */
            copyout(t->mm, umsg + 48, &mf, 4);
        }
    }
    rs_kfree(kb);
    return r;
}

static long inet_sockopt(struct task *t, struct isock *s, long level, long opt, uint64_t uval, uint64_t ulen)
{
    int v = 0;
    if (level == 1) {
        if (opt == 3) v = s->type == INET_STREAM ? 1 : 2;      /* SO_TYPE */
        else if (opt == 7 || opt == 8) v = 65536;              /* SO_SNDBUF / SO_RCVBUF */
        else if (opt == 39) v = 2;                             /* SO_DOMAIN */
        else if (opt == 38) v = s->type == INET_STREAM ? 6 : 17;   /* SO_PROTOCOL */
    }
    uint32_t l = 4;
    copyout(t->mm, uval, &v, 4);
    return copyout(t->mm, ulen, &l, 4);
}

/* alarm, setitimer, getitimer: ITIMER_REAL only (SIGALRM), in timer ticks */
static uint64_t tv_ticks(const int64_t tv[2]) { return (uint64_t)tv[0] * AUXO_HZ + ((uint64_t)tv[1] * AUXO_HZ + 999999) / 1000000; }
static void ticks_tv(uint64_t k, int64_t tv[2]) { tv[0] = (int64_t)(k / AUXO_HZ); tv[1] = (int64_t)(k % AUXO_HZ * (1000000 / AUXO_HZ)); }

static long sys_alarm(struct task *t, uint64_t sec)
{
    uint64_t now = kernel_ticks();
    long left = t->alarm_at > now ? (long)((t->alarm_at - now + AUXO_HZ - 1) / AUXO_HZ) : 0;
    t->alarm_every = 0;
    t->alarm_at = sec ? now + sec * AUXO_HZ : 0;
    return left;
}

static long sys_itimer(struct task *t, long which, uint64_t unew, uint64_t uold, int set)
{
    if (which != 0) return set ? 0 : -EINVAL;              /* VIRTUAL/PROF: accepted, never fire */
    uint64_t now = kernel_ticks();
    if (uold) {
        int64_t v[4];
        ticks_tv(t->alarm_every, v);
        ticks_tv(t->alarm_at > now ? t->alarm_at - now : 0, v + 2);
        if (copyout(t->mm, uold, v, 32) < 0) return -EFAULT;
    }
    if (!set) return 0;
    int64_t v[4];
    if (copyin(t->mm, v, unew, 32) < 0) return -EFAULT;
    uint64_t first = tv_ticks(v + 2);
    t->alarm_every = tv_ticks(v);
    t->alarm_at = first ? now + first : 0;
    return 0;
}

/* sendmmsg / recvmmsg: glibc's resolver asks for A and AAAA in one call */
static long sys_sendmsg(struct task *t, long fd, uint64_t umsg, long flags);
static long sys_recvmsg(struct task *t, long fd, uint64_t umsg, long flags);
static long sys_mmsg(struct task *t, long fd, uint64_t vec, long vlen, long flags, int out)
{
    long n = 0;
    for (; n < vlen && n < 64; n++) {
        long r = out ? sys_sendmsg(t, fd, vec + (uint64_t)n * 64, flags) : sys_recvmsg(t, fd, vec + (uint64_t)n * 64, n ? flags | 0x40 : flags);
        if (r < 0) return n ? n : r;
        uint32_t len = (uint32_t)r;
        copyout(t->mm, vec + (uint64_t)n * 64 + 56, &len, 4);
    }
    return n;
}

/* ── sockets (AF_UNIX) ──────────────────────────────────────────────── */
static struct usock *getsock(struct task *t, long fd, long *err)
{
    *err = 0;
    struct file *f = getf(t, fd);
    if (!f) { *err = -EBADF; return 0; }
    if (f->kind != 5) { *err = -ENOTSOCK; return 0; }
    return f->priv;
}

static long sock_fd(struct task *t, struct usock *u, long flags)
{
    struct file *f = file_new(5, 0, 2 | (int)(flags & 04000));
    if (!f) { unix_close(u); return -ENOMEM; }
    f->priv = u;
    strlcpy(f->path, "socket:[unix]", sizeof f->path);
    int fd = alloc_fd(t, f, 0, (flags & 02000000) != 0);
    if (fd < 0) file_put(f);
    return fd;
}

int nl_install(struct task *t, struct file *f, int cloexec)
{
    int fd = alloc_fd(t, f, 0, cloexec);
    if (fd < 0) file_put(f);
    return fd;
}

static struct file *getnl(struct task *t, long fd) { struct file *f = getf(t, fd); return f && f->kind == 10 ? f : 0; }

static long sys_socket(struct task *t, long domain, long type, long proto)
{
    if (domain == 16 && net_isolated(t)) return netlink_socket(t, type, proto);
    if (domain == 2) return inet_socket(t, type, proto);
    if (domain != 1) return -EAFNOSUPPORT;            /* AF_UNIX and AF_INET; IPv6 programs fall back */
    int st = (int)(type & 0xF);
    if (st != 1 && st != 2 && st != 5) return -94;               /* ESOCKTNOSUPPORT */
    struct usock *u = unix_new();
    if (!u) return -ENOMEM;
    u->type = st;
    return sock_fd(t, u, type);
}

static long sun_path(struct task *t, uint64_t uaddr, uint64_t len, char *out)
{
    uint8_t sa[110];
    memset(sa, 0, sizeof sa);
    if (len < 3 || len > sizeof sa) return -EINVAL;
    if (copyin(t->mm, sa, uaddr, len) < 0) return -EFAULT;
    if (sa[0] != 1) return -EAFNOSUPPORT;
    memcpy(out, sa + 2, len - 2);
    out[len - 2] = 0;
    return 0;
}

static long sys_bind(struct task *t, long fd, uint64_t ua, uint64_t len)
{
    if (getnl(t, fd)) return nl_bind(t, getnl(t, fd)->priv, ua, len);
    if (getisock(t, fd)) { uint32_t ip; uint16_t port; int fam; long e = sin_in(t, ua, len, &ip, &port, &fam); return e < 0 ? e : inet_bind(getisock(t, fd), ip, port); }
    long r; struct usock *u = getsock(t, fd, &r); if (!u) return r;
    char p[110];
    if ((r = sun_path(t, ua, len, p)) < 0) return r;
    if (!p[0]) return unix_bind_abstract(u, p + 1, t->ns.net->inum);
    struct path pd; char name[256];
    if ((r = at_parent(t, AT_FDCWD, p, &pd, name, sizeof name)) < 0) return r;
    return unix_bind_at(u, pd.n, name);
}

static long sys_connect(struct task *t, long fd, uint64_t ua, uint64_t len)
{
    if (getisock(t, fd)) { uint32_t ip; uint16_t port; int fam; long e = sin_in(t, ua, len, &ip, &port, &fam);
                           if (e < 0) return e;
                           if (net_isolated(t)) return isolated_dest(t, ip);
                           return inet_connect(getisock(t, fd), fam ? ip : 0, port); }
    long r; struct usock *u = getsock(t, fd, &r); if (!u) return r;
    char p[110];
    if ((r = sun_path(t, ua, len, p)) < 0) return r;
    if (!p[0]) return unix_connect_abstract(u, p + 1, t->ns.net->inum);
    struct path sp;
    if (vfs_walk(t, t->ns.cwd, p, 1, &sp) < 0) return -ECONNREFUSED;
    return unix_connect_node(u, sp.n);
}

static long sys_accept(struct task *t, long fd, long flags)
{
    if (getisock(t, fd)) return -EOPNOTSUPP;          /* TCP: clients only */
    long r; struct usock *u = getsock(t, fd, &r); if (!u) return r;
    struct file *lf = getf(t, fd);
    struct usock *c;
    while (!(c = unix_accept(u))) {
        if (lf->flags & 04000) return -EAGAIN;
        if (t->pending & ~t->sigmask) return -EINTR;
        wait_tick();
    }
    return sock_fd(t, c, flags);
}

static long sys_socketpair(struct task *t, long type, uint64_t usv)
{
    struct usock *a, *b;
    int st = (int)(type & 0xF);
    if (st != 1 && st != 2 && st != 5) return -94;
    if (unix_pair(&a, &b) < 0) return -ENOMEM;
    a->type = b->type = st;
    long fa = sock_fd(t, a, type);
    if (fa < 0) { unix_close(b); return fa; }
    long fb = sock_fd(t, b, type);
    if (fb < 0) { sys_close(t, fa); return fb; }
    int sv[2] = { (int)fa, (int)fb };
    return copyout(t->mm, usv, sv, 8);
}

static long nl_msg(struct task *t, long fd, uint64_t umsg, long flags, int out)
{
    struct file *f = getnl(t, fd);
    uint64_t m[7];
    if (copyin(t->mm, m, umsg, 56) < 0) return -EFAULT;
    uint8_t *kb = kbuf_get();
    if (!kb) return -ENOMEM;
    long r = 0;
    size_t total = 0;
    uint64_t iov[16][2], niov = m[3] < 16 ? m[3] : 16;
    for (uint64_t i = 0; i < niov; i++) if (copyin(t->mm, iov[i], m[2] + i * 16, 16) < 0) { rs_kfree(kb); return -EFAULT; }
    if (out) {
        for (uint64_t i = 0; i < niov; i++) {
            size_t c = iov[i][1] < KBUF - total ? iov[i][1] : KBUF - total;
            if (c && copyin(t->mm, kb + total, iov[i][0], c) < 0) { rs_kfree(kb); return -EFAULT; }
            total += c;
        }
        r = nl_send(t, f->priv, kb, total);
    } else {
        for (uint64_t i = 0; i < niov; i++) total += iov[i][1];
        if (total > KBUF) total = KBUF;
        for (;;) {
            r = nl_recv(f->priv, kb, total, (flags & 2) != 0);
            if (r != -EAGAIN || (flags & 0x40) || (f->flags & 04000)) break;
            if (t->pending & ~t->sigmask) { r = -EINTR; break; }
            wait_tick();
        }
        size_t o = 0;
        for (uint64_t i = 0; r > 0 && i < niov && o < (size_t)r; i++) {
            size_t c = (size_t)r - o < iov[i][1] ? (size_t)r - o : iov[i][1];
            copyout(t->mm, iov[i][0], kb + o, c);
            o += c;
        }
        if (r >= 0) {
            if (m[0] && m[1] >= 12) { uint8_t sa[12] = { 16, 0 }; copyout(t->mm, m[0], sa, 12); uint32_t l = 12; copyout(t->mm, umsg + 8, &l, 4); }
            uint64_t zero = 0; int mf = 0;
            copyout(t->mm, umsg + 40, &zero, 8);
            copyout(t->mm, umsg + 48, &mf, 4);
        }
    }
    rs_kfree(kb);
    return r;
}

static long sys_sendmsg(struct task *t, long fd, uint64_t umsg, long flags)
{
    if (getnl(t, fd)) return nl_msg(t, fd, umsg, flags, 1);
    if (getisock(t, fd)) return inet_msg(t, fd, umsg, flags, 1);
    long r; struct usock *u = getsock(t, fd, &r); if (!u) return r;
    struct file *sf = getf(t, fd);
    uint64_t m[7];
    if (copyin(t->mm, m, umsg, 56) < 0) return -EFAULT;
    /* passed descriptors */
    struct file *files[UNIX_MAXFDS];
    int nf = 0;
    if (m[4] && m[5] >= 16) {
        uint8_t ctl[512];
        size_t cl = m[5] < sizeof ctl ? (size_t)m[5] : sizeof ctl;
        if (copyin(t->mm, ctl, m[4], cl) < 0) return -EFAULT;
        for (size_t o = 0; o + 16 <= cl; ) {
            uint64_t len; int lvl, type;
            memcpy(&len, ctl + o, 8); memcpy(&lvl, ctl + o + 8, 4); memcpy(&type, ctl + o + 12, 4);
            if (len < 16 || o + len > cl) break;
            if (lvl == 1 && type == 1)
                for (size_t k = 16; k + 4 <= len && nf < UNIX_MAXFDS; k += 4) {
                    int pfd; memcpy(&pfd, ctl + o + k, 4);
                    struct file *pf = getf(t, pfd);
                    if (!pf) return -EBADF;
                    files[nf++] = pf;
                }
            o += (len + 7) & ~7ULL;
        }
    }
    uint8_t *kb = kbuf_get();
    if (!kb) return -ENOMEM;
    size_t total = 0;
    for (uint64_t i = 0; i < m[3] && i < 64; i++) {
        uint64_t iov[2];
        if (copyin(t->mm, iov, m[2] + i * 16, 16) < 0) { rs_kfree(kb); return -EFAULT; }
        if (total + iov[1] > KBUF) { rs_kfree(kb); return -EMSGSIZE_; }
        if (copyin(t->mm, kb + total, iov[0], iov[1]) < 0) { rs_kfree(kb); return -EFAULT; }
        total += iov[1];
    }
    size_t done = 0;
    r = 0;
    for (;;) {
        r = unix_send(u, kb + done, total - done, done ? 0 : files, done ? 0 : nf);
        if (r > 0) { done += (size_t)r; if (done == total) break; continue; }
        if (r == -EAGAIN && !(flags & 0x40) && !(sf->flags & 04000)) { wait_tick(); continue; }
        break;
    }
    rs_kfree(kb);
    if (!done && r < 0) { if (r == -EPIPE && !(flags & 0x4000)) t->pending |= 1ULL << 12; return r; }
    return (long)done;
}

static long sys_recvmsg(struct task *t, long fd, uint64_t umsg, long flags)
{
    if (getnl(t, fd)) return nl_msg(t, fd, umsg, flags, 0);
    if (getisock(t, fd)) return inet_msg(t, fd, umsg, flags, 0);
    long r; struct usock *u = getsock(t, fd, &r); if (!u) return r;
    struct file *sf = getf(t, fd);
    uint64_t m[7];
    if (copyin(t->mm, m, umsg, 56) < 0) return -EFAULT;
    size_t want = 0;
    uint64_t iov[64][2];
    uint64_t niov = m[3] < 64 ? m[3] : 64;
    for (uint64_t i = 0; i < niov; i++) { if (copyin(t->mm, iov[i], m[2] + i * 16, 16) < 0) return -EFAULT; want += iov[i][1]; }
    if (want > KBUF) want = KBUF;
    uint8_t *kb = kbuf_get();
    if (!kb) return -ENOMEM;
    struct file *files[UNIX_MAXFDS];
    int nf = 0;
    for (;;) {
        r = unix_recv(u, kb, want, files, &nf, UNIX_MAXFDS);
        if (r != -EAGAIN || (flags & 0x40) || (sf->flags & 04000)) break;
        if (t->pending & ~t->sigmask) { r = -EINTR; break; }
        wait_tick();
    }
    if (r < 0) { rs_kfree(kb); return r; }
    size_t o = 0;
    for (uint64_t i = 0; i < niov && o < (size_t)r; i++) {
        size_t c = (size_t)r - o < iov[i][1] ? (size_t)r - o : iov[i][1];
        if (copyout(t->mm, iov[i][0], kb + o, c) < 0) { rs_kfree(kb); return -EFAULT; }
        o += c;
    }
    rs_kfree(kb);
    uint64_t clen = 0;
    int mflags = 0;
    if (nf) {
        uint8_t ctl[16 + 4 * UNIX_MAXFDS];
        uint64_t len = 16 + 4 * (uint64_t)nf;
        int lvl = 1, type = 1;
        memcpy(ctl, &len, 8); memcpy(ctl + 8, &lvl, 4); memcpy(ctl + 12, &type, 4);
        for (int i = 0; i < nf; i++) {
            int nfd = alloc_fd(t, files[i], 0, (flags & 0x40000000) != 0);   /* MSG_CMSG_CLOEXEC */
            if (nfd < 0) { file_put(files[i]); nfd = -1; }
            memcpy(ctl + 16 + 4 * i, &nfd, 4);
        }
        if (m[5] >= len) { copyout(t->mm, m[4], ctl, len); clen = (len + 7) & ~7ULL; }
        else mflags |= 8;                                    /* MSG_CTRUNC */
    }
    copyout(t->mm, umsg + 40, &clen, 8);
    copyout(t->mm, umsg + 48, &mflags, 4);
    uint32_t zero = 0;
    if (m[0]) copyout(t->mm, umsg + 8, &zero, 4);
    return r;
}

static long sys_sockopt(struct task *t, long fd, long level, long opt, uint64_t uval, uint64_t ulen)
{
    if (getnl(t, fd)) { int v = opt == 3 ? 3 : 32768; uint32_t l = 4; copyout(t->mm, uval, &v, 4); return copyout(t->mm, ulen, &l, 4); }
    if (getisock(t, fd)) return inet_sockopt(t, getisock(t, fd), level, opt, uval, ulen);
    long r; struct usock *u = getsock(t, fd, &r); if (!u) return r;
    if (level != 1) return -ENOPROTOOPT_;
    int v = 0;
    if (opt == 17) {                                        /* SO_PEERCRED */
        int cred[3] = { 1, 0, 0 };
        uint32_t l = 12;
        copyout(t->mm, uval, cred, 12);
        return copyout(t->mm, ulen, &l, 4);
    }
    if (opt == 3) v = u->type ? u->type : 1;                /* SO_TYPE */
    else if (opt == 7 || opt == 8) v = 256 * 1024;          /* buffers */
    else if (opt == 30) v = 1;                              /* SO_ACCEPTCONN etc. */
    uint32_t l = 4;
    copyout(t->mm, uval, &v, 4);
    return copyout(t->mm, ulen, &l, 4);
}

static long sys_sockname(struct task *t, long fd, uint64_t ua, uint64_t ulen, int peer)
{
    if (getnl(t, fd)) return nl_name(t, getnl(t, fd)->priv, ua, ulen);
    if (getisock(t, fd)) {
        uint32_t lip, rip; uint16_t lp, rp;
        inet_names(getisock(t, fd), &lip, &lp, &rip, &rp);
        if (peer && !getisock(t, fd)->connected) return -107;      /* ENOTCONN */
        return peer ? sin_out(t, ua, ulen, rip, rp) : sin_out(t, ua, ulen, lip, lp);
    }
    long r; struct usock *u = getsock(t, fd, &r); if (!u) return r;
    (void)u;
    uint16_t fam = 1;
    uint32_t l = 2;
    if (ua) copyout(t->mm, ua, &fam, 2);
    if (ulen) copyout(t->mm, ulen, &l, 4);
    return 0;
}

/* epoll: an interest list polled like poll(2); level-triggered (EPOLLONESHOT honoured) */
struct epoll { int n; struct { int fd; uint32_t events; uint64_t data; } e[64]; };

static long sys_epoll_create(struct task *t, long flags)
{
    struct epoll *ep = rs_kmalloc(sizeof *ep);
    if (!ep) return -ENOMEM;
    memset(ep, 0, sizeof *ep);
    struct file *f = file_new(7, 0, 2);
    if (!f) { rs_kfree(ep); return -ENOMEM; }
    f->priv = ep;
    strlcpy(f->path, "anon_inode:[eventpoll]", sizeof f->path);
    int fd = alloc_fd(t, f, 0, (flags & 02000000) != 0);
    if (fd < 0) file_put(f);
    return fd;
}

static long sys_epoll_ctl(struct task *t, long epfd, long op, long fd, uint64_t uev)
{
    struct file *f = getf(t, epfd);
    if (!f || f->kind != 7) return -EBADF;
    struct epoll *ep = f->priv;
    if (!getf(t, fd)) return -EBADF;
    struct { uint32_t events; uint64_t data; } __attribute__((packed)) ev = { 0, 0 };
    if (op != 2 && copyin(t->mm, &ev, uev, 12) < 0) return -EFAULT;
    int i;
    for (i = 0; i < ep->n; i++) if (ep->e[i].fd == fd) break;
    if (op == 1) {                                               /* ADD */
        if (i < ep->n) return -EEXIST;
        if (ep->n >= 64) return -ENOMEM;
        ep->e[ep->n].fd = (int)fd; ep->e[ep->n].events = ev.events; ep->e[ep->n].data = ev.data; ep->n++;
        return 0;
    }
    if (i == ep->n) return -ENOENT;
    if (op == 2) { ep->e[i] = ep->e[--ep->n]; return 0; }       /* DEL */
    ep->e[i].events = ev.events; ep->e[i].data = ev.data;       /* MOD */
    return 0;
}

static long sys_epoll_wait(struct task *t, long epfd, uint64_t uev, long max, int64_t timeout_ms)
{
    struct file *f = getf(t, epfd);
    if (!f || f->kind != 7) return -EBADF;
    struct epoll *ep = f->priv;
    if (max <= 0) return -EINVAL;
    uint64_t until = timeout_ms < 0 ? ~0ULL : kernel_ticks() + (uint64_t)(timeout_ms + 9) / 10;
    for (;;) {
        int n = 0;
        for (int i = 0; i < ep->n && n < max; i++) {
            uint32_t want = ep->e[i].events & 0x7FF;
            if (!want) continue;
            int r = fd_ready(t, ep->e[i].fd, (int)(want & 0xFF));
            if (r & 0x20) { ep->e[i] = ep->e[--ep->n]; i--; continue; }   /* closed fd: forget it */
            if (!r) continue;
            struct { uint32_t events; uint64_t data; } __attribute__((packed)) out = { (uint32_t)r, ep->e[i].data };
            if (copyout(t->mm, uev + (uint64_t)n * 12, &out, 12) < 0) return -EFAULT;
            n++;
            if (ep->e[i].events & (1u << 30)) ep->e[i].events = 0;          /* EPOLLONESHOT */
        }
        if (n || kernel_ticks() >= until) return n;
        if (t->pending & ~t->sigmask) return -EINTR;
        wait_tick();
    }
}

static long sys_signalfd(struct task *t, long fd, uint64_t umask, uint64_t size, long flags)
{
    uint64_t mask;
    if (size != 8) return -EINVAL;
    if (copyin(t->mm, &mask, umask, 8) < 0) return -EFAULT;
    mask &= ~((1ULL << 8) | (1ULL << 18));                       /* never SIGKILL, SIGSTOP */
    if (fd != -1) {
        struct file *f = getf(t, fd);
        if (!f || f->kind != 9) return -EINVAL;
        *(uint64_t *)f->priv = mask;
        return fd;
    }
    uint64_t *m = rs_kmalloc(8);
    if (!m) return -ENOMEM;
    *m = mask;
    struct file *f = file_new(9, 0, 2 | (int)(flags & 04000));
    if (!f) { rs_kfree(m); return -ENOMEM; }
    f->priv = m;
    strlcpy(f->path, "anon_inode:[signalfd]", sizeof f->path);
    int nfd = alloc_fd(t, f, 0, (flags & 02000000) != 0);
    if (nfd < 0) file_put(f);
    return nfd;
}

static long sys_eventfd(struct task *t, uint64_t init, long flags)
{
    uint64_t *c = rs_kmalloc(8);
    if (!c) return -ENOMEM;
    *c = init;
    struct file *f = file_new(6, 0, 2 | (int)(flags & 04000));
    if (!f) { rs_kfree(c); return -ENOMEM; }
    f->priv = c;
    strlcpy(f->path, "anon_inode:[eventfd]", sizeof f->path);
    int fd = alloc_fd(t, f, 0, (flags & 02000000) != 0);
    if (fd < 0) file_put(f);
    return fd;
}

/* ── the table ──────────────────────────────────────────────────────── */
static long syscall_body(struct task *t, struct trapframe *tf)
{
    uint64_t a = tf->rdi, b = tf->rsi, c = tf->rdx, d = tf->r10, e = tf->r8, f = tf->r9;
    long nr = (long)tf->rax;
    switch (nr) {
    case 0: return sys_read(t, (long)a, b, c);
    case 1: return sys_write(t, (long)a, b, c);
    case 2: return sys_openat(t, AT_FDCWD, a, (long)b, (long)c);
    case 3: return sys_close(t, (long)a);
    case 85: return sys_openat(t, AT_FDCWD, a, 0100 | 01 | 01000, (long)b);   /* creat */
    case 4: return sys_fstatat(t, AT_FDCWD, a, b, 0, 0);
    case 5: { char z = 0; (void)z; struct file *fl = getf(t, (long)a); if (!fl) return -EBADF;
              if (!fl->node) { struct node tmp; memset(&tmp, 0, sizeof tmp); tmp.type = N_FIFO; tmp.mode = 0600; tmp.ino = 77; return do_stat_node(t, &tmp, b); }
              return do_stat_node(t, fl->node, b); }
    case 6: return sys_fstatat(t, AT_FDCWD, a, b, 0x100, 0);
    case 7: return sys_poll(t, a, (long)b, (int64_t)(int)c);
    case 8: return sys_lseek(t, (long)a, (int64_t)b, (long)c);
    case 9: { mm_lock(t->mm); long r = sys_mmap(t, a, b, (long)c, (long)d, (long)(int)e, f); mm_unlock(t->mm); return r; }
    case 10: return mm_protect(t->mm, a, b, (uint32_t)(c & 7));
    case 11: return mm_unmap(t->mm, a, b);
    case 12: { mm_lock(t->mm); long r = sys_brk(t, a); mm_unlock(t->mm); return r; }
    case 13: {                                                 /* rt_sigaction */
        if (a < 1 || a > 64) return -EINVAL;
        struct sigact *sa = &t->sig[a - 1];
        if (c && copyout(t->mm, c, sa, 32) < 0) return -EFAULT;
        if (b && a != 9 && a != 19) { if (copyin(t->mm, sa, b, 32) < 0) return -EFAULT; }
        return 0;
    }
    case 14: {                                                 /* rt_sigprocmask */
        uint64_t old = t->sigmask, set;
        if (b) {
            if (copyin(t->mm, &set, b, 8) < 0) return -EFAULT;
            if (a == 0) t->sigmask |= set; else if (a == 1) t->sigmask &= ~set; else t->sigmask = set;
            t->sigmask &= ~((1ULL << 8) | (1ULL << 18));
        }
        if (c && copyout(t->mm, c, &old, 8) < 0) return -EFAULT;
        return 0;
    }
    case 15: return sys_rt_sigreturn(t, tf);
    case 16: return sys_ioctl(t, (long)a, b, c);
    case 17: return sys_pread(t, (long)a, b, c, (int64_t)d, 0);
    case 18: return sys_pread(t, (long)a, b, c, (int64_t)d, 1);
    case 19: return sys_rwv(t, (long)a, b, (long)c, 0, -1);
    case 20: return sys_rwv(t, (long)a, b, (long)c, 1, -1);
    case 21: return sys_faccessat(t, AT_FDCWD, a, (long)b);
    case 22: return sys_pipe2(t, a, 0);
    case 23: { int64_t ms = -1; if (e) { int64_t tv[2]; if (copyin(t->mm, tv, e, 16) == 0) ms = tv[0] * 1000 + tv[1] / 1000; } return sys_select(t, (long)a, b, c, d, ms); }
    case 24: rs_yield(); return 0;
    case 25: { mm_lock(t->mm); long r = sys_mremap(t, a, b, c, (long)d); mm_unlock(t->mm); return r; }
    case 26: case 27: return 0;                               /* msync, mincore */
    case 28:                                                   /* madvise */
        if (c == 4 || c == 8 || c == 9) return mm_discard(t->mm, a, b);   /* DONTNEED, FREE, REMOVE */
        return 0;
    case 32: { struct file *fl = getf(t, (long)a); if (!fl) return -EBADF; int n = alloc_fd(t, fl, 0, 0); if (n >= 0) fl->refs++; return n; }
    case 33: return sys_dup3(t, (long)a, (long)b, 0);
    case 35: return sys_nanosleep(t, a);
    case 39: return pid_vtgid(t, t);
    case 41: return sys_socket(t, (long)a, (long)b, (long)c);
    case 42: return sys_connect(t, (long)a, b, c);
    case 43: return sys_accept(t, (long)a, 0);
    case 288: return sys_accept(t, (long)a, (long)d);
    case 44: if (getisock(t, (long)a)) return inet_sendto(t, (long)a, b, c, (long)d, e, f); return sys_write(t, (long)a, b, c);
    case 45: if (getisock(t, (long)a)) return inet_recvfrom(t, (long)a, b, c, (long)d, e, f); return sys_read(t, (long)a, b, c);
    case 46: return sys_sendmsg(t, (long)a, b, (long)c);
    case 47: return sys_recvmsg(t, (long)a, b, (long)c);
    case 48: if (getisock(t, (long)a)) { inet_shutdown(getisock(t, (long)a), (int)b); return 0; }
             { long r; struct usock *u = getsock(t, (long)a, &r); if (!u) return r; if (u->peer) u->peer->peer_closed = 1; return 0; }
    case 49: return sys_bind(t, (long)a, b, c);
    case 50: if (getisock(t, (long)a)) return -EOPNOTSUPP;
             { long r; struct usock *u = getsock(t, (long)a, &r); if (!u) return r; return unix_listen(u); }
    case 51: case 52: return sys_sockname(t, (long)a, b, c, nr == 52);
    case 37: return sys_alarm(t, a);
    case 36: return sys_itimer(t, (long)a, 0, b, 0);
    case 38: return sys_itimer(t, (long)a, b, c, 1);
    case 307: return sys_mmsg(t, (long)a, b, (long)c, (long)d, 1);
    case 299: return sys_mmsg(t, (long)a, b, (long)c, (long)d, 0);
    case 53: return sys_socketpair(t, (long)b, d);
    case 54: return 0;
    case 55: return sys_sockopt(t, (long)a, (long)b, (long)c, d, e);
    case 56: return sys_clone(t, a, b, c, d, e);
    case 57: return sys_clone(t, 17, 0, 0, 0, 0);
    case 58: return sys_clone(t, 0x4000 | 0x100 | 17, 0, 0, 0, 0);
    case 59: return sys_execve(t, a, b, c);
    case 60: task_exit(t, (int)(a & 0xFF) << 8);
    case 61: return sys_wait4(t, (long)(int)a, b, (long)c);
    case 62: return sys_kill(t, (long)(int)a, (long)b);
    case 63: return sys_uname(t, a);
    case 72: return sys_fcntl(t, (long)a, (long)b, c);
    case 73: case 74: case 75: return 0;                       /* flock, fsync, fdatasync */
    case 162: case 306: store_save(); return 0;               /* sync, syncfs: an installed system writes its disk now */
    case 76: { char p[1024]; struct path pp; long r = upath(t, a, p, sizeof p); if (r < 0) return r;
               if ((r = vfs_walk(t, t->ns.cwd, p, 1, &pp)) < 0) return r;
               if (vfs_readonly(pp.m)) return -EROFS;
               return sys_truncate(pp.n, (int64_t)b); }
    case 77: { struct file *fl = getf(t, (long)a); if (!fl || !fl->node) return -EBADF;
               if ((fl->flags & 3) == 0) return -EINVAL;
               if (vfs_readonly(fl->mnt)) return -EROFS;
               return sys_truncate(fl->node, (int64_t)b); }
    case 79: return sys_getcwd(t, a, b);
    case 80: return sys_chdir(t, a);
    case 81: { struct file *fl = getf(t, (long)a); if (!fl || !fl->node) return -EBADF; if (fl->node->type != N_DIR) return -ENOTDIR;
               t->cwd = fl->node; t->ns.cwd.m = fl->mnt ? fl->mnt : mount_rootm(); t->ns.cwd.n = fl->node; return 0; }
    case 82: return sys_renameat(t, AT_FDCWD, a, AT_FDCWD, b);
    case 83: return sys_mkdirat(t, AT_FDCWD, a, (long)b);
    case 84: return sys_unlinkat(t, AT_FDCWD, a, 0x200);
    case 86: return sys_linkat(t, AT_FDCWD, a, AT_FDCWD, b);
    case 87: return sys_unlinkat(t, AT_FDCWD, a, 0);
    case 88: return sys_symlinkat(t, a, AT_FDCWD, b);
    case 89: return sys_readlinkat(t, AT_FDCWD, a, b, c);
    case 90: { char p[1024]; struct path pp; long r = upath(t, a, p, sizeof p); if (r < 0) return r;
               if ((r = vfs_walk(t, t->ns.cwd, p, 1, &pp)) < 0) return r;
               if (vfs_readonly(pp.m) || procfile(pp.n)) return vfs_readonly(pp.m) ? -EROFS : -EPERM;
               pp.n->mode = (uint16_t)(b & 07777); fs_touch(pp.n); return 0; }
    case 91: { struct file *fl = getf(t, (long)a); if (!fl || !fl->node) return -EBADF;
               if (vfs_readonly(fl->mnt)) return -EROFS;
               fl->node->mode = (uint16_t)(b & 07777); fs_touch(fl->node); return 0; }
    case 268: { char p[1024]; struct path pp; long r = upath(t, b, p, sizeof p); if (r < 0) return r;
                if ((r = at_walk(t, (long)(int)a, p, 1, &pp)) < 0) return r;
                if (vfs_readonly(pp.m) || procfile(pp.n)) return vfs_readonly(pp.m) ? -EROFS : -EPERM;
                pp.n->mode = (uint16_t)(c & 07777); fs_touch(pp.n); return 0; }
    case 92: case 93: case 94: case 260: return 0;             /* chown: everything is root's */
    case 95: { long o = t->umask; t->umask = (uint32_t)(a & 0777); return o; }
    case 96: { int64_t s, ns; now_ts(0, &s, &ns); int64_t tv[2] = { s, ns / 1000 }; return a ? copyout(t->mm, a, tv, 16) : 0; }
    case 97: return sys_prlimit(t, (long)a, 0, b);
    case 98: { uint8_t ru[144]; memset(ru, 0, sizeof ru); return copyout(t->mm, b, ru, sizeof ru); }
    case 99: return sys_sysinfo(t, a);
    case 100: { uint64_t tm[4] = { 0 }; tm[0] = (kernel_ticks() - t->start_tick); if (a) copyout(t->mm, a, tm, 32); return (long)kernel_ticks(); }
    case 102: case 104: case 107: case 108: return 0;          /* uid/gid: root */
    case 105: case 106: case 113: case 114: case 116: case 117: case 119: return 0;
    case 118: case 120: { uint32_t z = 0; if (a) copyout(t->mm, a, &z, 4); if (b) copyout(t->mm, b, &z, 4); if (c) copyout(t->mm, c, &z, 4); return 0; }
    case 109: { struct task *o = a ? task_by_pid(pid_global(t, (int)a, 1)) : t; if (!o) return -ESRCH;
                int pg = b ? pid_global(t, (int)b, 1) : o->tgid; if (!pg) return -EPERM;
                o->pgid = pg; return 0; }
    case 110: { struct task *pp = t->ppid > 0 ? task_by_pid(t->ppid) : 0;
                if (pp) return pid_vtgid(t, pp);
                return t->ns.pid && t->ns.pid->level ? 0 : 1; }
    case 111: { struct task *l = task_by_pid(t->pgid); return l ? pid_vtgid(t, l) : t->pgid; }
    case 112: t->pgid = t->tgid; return pid_vtgid(t, t);
    case 115: return 0;                                        /* getgroups */
    case 121: { struct task *o = a ? task_by_pid(pid_global(t, (int)a, 1)) : t; if (!o) return -ESRCH;
                struct task *l = task_by_pid(o->pgid); return l ? pid_vtgid(t, l) : o->pgid; }
    case 124: return pid_vtgid(t, t);
    case 127: { uint64_t z = t->pending; return copyout(t->mm, a, &z, 8); }
    case 130: { uint64_t m; if (copyin(t->mm, &m, a, 8) < 0) return -EFAULT; uint64_t old = t->sigmask; t->sigmask = m;
                while (!(t->pending & ~t->sigmask)) wait_tick();
                t->sigmask = old;
                return -EINTR; }
    case 131:
        if (b) { uint8_t ss[24]; memset(ss, 0, sizeof ss); int fl = 2; memcpy(ss + 8, &fl, 4); copyout(t->mm, b, ss, 24); }
        return 0;
    case 137: case 138: { uint64_t sf[15]; memset(sf, 0, sizeof sf); sf[0] = 0x01021994; sf[1] = 4096; sf[2] = sf[3] = sf[4] = pmm_free_pages(); sf[5] = sf[6] = 100000; sf[8] = 255; sf[9] = 4096; return copyout(t->mm, b, sf, 120); }
    case 157: return sys_prctl(t, (long)a, b, c, d, e);
    case 125: return sys_capget(t, a, b);
    case 126: return sys_capset(t, a, b);
    case 155: return sys_pivot_root(t, a, b);
    case 161: return sys_chroot(t, a);
    case 165: return sys_mount(t, a, b, c, d, e);
    case 166: return sys_umount2(t, a, (long)b);
    case 170: return sys_sethostname(t, a, b, 0);
    case 171: return sys_sethostname(t, a, b, 1);
    case 272: return sys_unshare(t, a);
    case 308: return sys_setns(t, (long)a, (long)b);
    case 317: return sys_seccomp(t, (long)a, (long)b, c);
    case 282: return sys_signalfd(t, (long)(int)a, b, c, 0);
    case 289: return sys_signalfd(t, (long)(int)a, b, c, (long)d);
    case 158:                                                  /* arch_prctl */
        if (a == 0x1002) { wrmsr(0xC0000100, b); return 0; }
        if (a == 0x1003) { uint64_t v = rdmsr(0xC0000100); return copyout(t->mm, b, &v, 8); }
        if (a == 0x3001) return -EINVAL;                       /* CET */
        return -EINVAL;
    case 160: return 0;                                        /* setrlimit */
    case 186: return pid_vpid(t, t);
    case 200: return sys_tkill(t, -1, (long)(int)a, (long)b);
    case 201: { int64_t s, ns; now_ts(0, &s, &ns); if (a) copyout(t->mm, a, &s, 8); return s; }
    case 202: return sys_futex(t, a, (long)b, c, d);
    case 203: return 0;
    case 204: { uint64_t mask = 1; if (b < 8) return -EINVAL; return copyout(t->mm, c, &mask, 8) < 0 ? -EFAULT : 8; }
    case 213: return sys_epoll_create(t, 0);
    case 291: return sys_epoll_create(t, (long)a);
    case 233: return sys_epoll_ctl(t, (long)a, (long)b, (long)c, d);
    case 232: case 281: case 441: return sys_epoll_wait(t, (long)a, b, (long)c, (int64_t)(int)d);
    case 221: return 0;                                        /* fadvise64 */
    case 436: {                                                /* close_range */
        uint64_t last = b < MAXFD ? b : MAXFD - 1;
        for (uint64_t fd = a; fd <= last; fd++) {
            if (!t->files->fd[fd]) continue;
            if (c & 4) t->files->cloexec[fd] = 1;
            else { file_put(t->files->fd[fd]); t->files->fd[fd] = 0; }
        }
        return 0;
    }
    case 217: return sys_getdents64(t, (long)a, b, c);
    case 218: t->clear_child_tid = a; return pid_vpid(t, t);
    case 228: { int64_t ts[2]; now_ts((int)a, &ts[0], &ts[1]); return copyout(t->mm, b, ts, 16); }
    case 229: { int64_t ts[2] = { 0, 10000000 }; return b ? copyout(t->mm, b, ts, 16) : 0; }
    case 230: { int64_t ts[2]; if (copyin(t->mm, ts, c, 16) < 0) return -EFAULT;
                if (b & 1) { int64_t s, ns; now_ts((int)a, &s, &ns); ts[0] -= s; ts[1] -= ns; if (ts[1] < 0) { ts[0]--; ts[1] += 1000000000; } if (ts[0] < 0) return 0; }
                uint64_t until = kernel_ticks() + (uint64_t)ts[0] * AUXO_HZ + (uint64_t)ts[1] / 10000000;
                while (kernel_ticks() < until) { if (t->pending & ~t->sigmask) return -EINTR; rs_sleep(until - kernel_ticks()); }
                return 0; }
    case 231: exit_group(t, (int)(a & 0xFF) << 8);
    case 234: return sys_tkill(t, (long)(int)a, (long)(int)b, (long)c);
    case 247: return -ECHILD;                                  /* waitid: rarely used; not yet */
    case 257: return sys_openat(t, (long)(int)a, b, (long)c, (long)d);
    case 258: return sys_mkdirat(t, (long)(int)a, b, (long)c);
    case 262: return sys_fstatat(t, (long)(int)a, b, c, (long)d, 0);
    case 263: return sys_unlinkat(t, (long)(int)a, b, (long)c);
    case 264: case 316: return sys_renameat(t, (long)(int)a, b, (long)(int)c, d);
    case 265: return sys_linkat(t, (long)(int)a, b, (long)(int)c, d);
    case 266: return sys_symlinkat(t, a, (long)(int)b, c);
    case 267: return sys_readlinkat(t, (long)(int)a, b, c, d);
    case 269: case 439: return sys_faccessat(t, (long)(int)a, b, (long)c);
    case 270: { int64_t ms = -1; if (e) { int64_t ts[2]; if (copyin(t->mm, ts, e, 16) == 0) ms = ts[0] * 1000 + ts[1] / 1000000; } return sys_select(t, (long)a, b, c, d, ms); }
    case 271: { int64_t ms = -1; if (c) { int64_t ts[2]; if (copyin(t->mm, ts, c, 16) == 0) ms = ts[0] * 1000 + ts[1] / 1000000; } return sys_poll(t, a, (long)b, ms); }
    case 273: case 274: return 0;                              /* robust lists */
    case 280: {                                                /* utimensat */
        struct node *un; struct mount *um;
        if (!b) { struct file *fl = getf(t, (long)(int)a); if (!fl || !fl->node) return -EBADF; un = fl->node; um = fl->mnt; }
        else { char p[1024]; struct path pp; long r = upath(t, b, p, sizeof p); if (r < 0) return r;
               if (!p[0] && (d & 0x1000)) { struct file *fl = getf(t, (long)(int)a); if (!fl || !fl->node) return -EBADF; pp.n = fl->node; pp.m = fl->mnt; }
               else if ((r = at_walk(t, (long)(int)a, p, !(d & 0x100), &pp)) < 0) return r;
               un = pp.n; um = pp.m; }
        if (vfs_readonly(um) || procfile(un)) return vfs_readonly(um) ? -EROFS : -EPERM;
        int64_t ts[4] = { 0, 0x3FFFFFFF, 0, 0x3FFFFFFF };       /* NULL: now */
        if (c && copyin(t->mm, ts, c, 32) < 0) return -EFAULT;
        if (ts[3] != 0x3FFFFFFE) { un->mtime = ts[3] == 0x3FFFFFFF ? fs_now() : (uint32_t)ts[2]; fs_touch(un); }
        return 0;
    }
    case 284: return sys_eventfd(t, a, 0);
    case 290: return sys_eventfd(t, a, (long)b);
    case 292: return sys_dup3(t, (long)a, (long)b, (long)c);
    case 293: return sys_pipe2(t, a, (long)b);
    case 285: { struct file *fl = getf(t, (long)a); if (!fl || !fl->node) return -EBADF;   /* fallocate */
                uint64_t want = c + d;
                if (!(b & 1) && want > fl->node->size) return fs_truncate(fl->node, want);
                return 0; }
    case 302: return sys_prlimit(t, (long)b, c, d);
    case 318: return sys_getrandom(t, a, b);
    case 319: return sys_memfd(t, a, (long)b);
    case 324: return 0;                                        /* membarrier */
    case 332: { char p[1024]; long r; struct node *dir, *n;
                if ((r = upath(t, b, p, sizeof p)) < 0) return r;
                if (!p[0] && (c & 0x1000)) { struct file *fl = getf(t, (long)(int)a); if (!fl) return -EBADF;
                    if (!fl->node) { struct node tmp; memset(&tmp, 0, sizeof tmp); tmp.type = N_FIFO; tmp.mode = 0600; return do_statx(t, &tmp, e); }
                    return do_statx(t, fl->node, e); }
                struct path pp;
                (void)dir; (void)n;
                if ((r = at_walk(t, (long)(int)a, p, !(c & 0x100), &pp)) < 0) return r;
                return do_statx(t, pp.n, e); }
    case 334: return -ENOSYS;                                  /* rseq */
    case 435: {                                                /* clone3 */
        uint64_t ca[11];
        memset(ca, 0, sizeof ca);
        if (b < 64 || copyin(t->mm, ca, a, b < 88 ? b : 88) < 0) return -EINVAL;
        uint64_t stack = ca[5] ? ca[5] + ca[6] : 0;
        return sys_clone(t, ca[0] | (ca[4] & 0xFF), stack, ca[3], ca[2], ca[7]);
    }
    }
    static uint32_t seen[16];
    if (nr >= 0 && nr < 512 && !(seen[nr / 32] & (1u << (nr % 32)))) {
        seen[nr / 32] |= 1u << (nr % 32);
        kprintf("[auxobsd: %s called unsupported syscall %ld]\n", t->name, nr);
    }
    return -ENOSYS;
}

int sys_trace;                              /* auxsh `strace`: log every system call */
char sys_trace_name[16];                   /* `strace -n NAME`: only these programs */

void syscall_dispatch(struct trapframe *tf)
{
    __asm__ volatile("sti");
    struct task *t = task_current();
    if (!t) { tf->rax = (uint64_t)-ENOSYS; return; }
    t->tf = tf;
    uint64_t nr = tf->rax, a0 = tf->rdi, a1 = tf->rsi, a2 = tf->rdx;
    char s0[48] = "";
    if (sys_trace && (nr == 2 || nr == 59 || nr == 4 || nr == 6 || nr == 21 || nr == 80 || nr == 89)) copyin_str(t->mm, s0, a0, sizeof s0);
    if (sys_trace && (nr == 257 || nr == 262 || nr == 267 || nr == 269 || nr == 332 || nr == 439)) copyin_str(t->mm, s0, a1, sizeof s0);
    long r;
    int run = 1;
    if (t->ns.seccomp_mode) {
        uint64_t args[6] = { tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9 };
        int k = seccomp_check(t, (long)nr, args, tf->rip, &r);
        if (k == 2) { if (sys_trace) kprintf("[%d %s] seccomp killed it at syscall %lu]\n", t->pid, t->name, nr); exit_group(t, 31); }
        if (k == 3) task_exit(t, 31);                                          /* SIGSYS */
        run = k == 1;
    }
    static uint64_t gc_at;
    if (kernel_ticks() - gc_at > 2 * AUXO_HZ) { gc_at = kernel_ticks(); procfs_gc(); }
    if (run) r = syscall_body(t, tf);
    if (sys_trace && (!sys_trace_name[0] || !strncmp(t->name, sys_trace_name, strlen(sys_trace_name))) &&
        (sys_trace == 1 || (r < 0 && r != -EAGAIN && nr != 61 && nr != 202))) kprintf("[%d %s] %lu(%lx, %lx, %lx%s%s%s) = %ld\n", t->pid, t->name, nr, a0, a1, a2, s0[0] ? ", \"" : "", s0, s0[0] ? "\"" : "", r);
    tf->rax = (uint64_t)r;
    uproc_signals(t, tf);
}

void user_exception(struct trapframe *tf)
{
    uint64_t a = read_cr2();                 /* before interrupts are back on: another fault would overwrite it */
    struct task *t = task_current();
    if (!t) panic("user exception without a task");
    __asm__ volatile("sti");
    if (tf->vector == 14) {
        if (mm_fault(t->mm, a, (tf->err & 2) != 0) == 0) return;
        t->pending |= 1ULL << 10;                               /* SIGSEGV */
        static int shown;
        if (shown++ < 3) {
            struct vma *v = mm_vma_at(t->mm, tf->rip);
            char fp[128] = "?";
            if (v && v->file) fs_path(v->file, fp, sizeof fp);
            kprintf("[%s: segfault at %lx (%s), rip %lx = %s+%lx]\n", t->name, a, tf->err & 2 ? "write" : "read", tf->rip, fp,
                    v ? tf->rip - v->start + v->off : 0);
            kprintf("[rax %lx rbx %lx rcx %lx rdx %lx rsi %lx rdi %lx rbp %lx rsp %lx r8 %lx r12 %lx r13 %lx r14 %lx r15 %lx]\n",
                    tf->rax, tf->rbx, tf->rcx, tf->rdx, tf->rsi, tf->rdi, tf->rbp, tf->rsp, tf->r8, tf->r12, tf->r13, tf->r14, tf->r15);
        }
    } else if (tf->vector == 6) t->pending |= 1ULL << 3;          /* SIGILL */
    else if (tf->vector == 0 || tf->vector == 16 || tf->vector == 19) t->pending |= 1ULL << 7;   /* SIGFPE */
    else if (tf->vector == 3) t->pending |= 1ULL << 4;            /* SIGTRAP */
    else {
        t->pending |= 1ULL << 10;
        kprintf("[%s: %s at rip %lx]\n", t->name, tf->vector == 13 ? "protection fault" : "CPU exception", tf->rip);
    }
    /* a fault the program has a handler for gets the handler; otherwise it dies */
    uproc_signals(t, tf);
}

/* returning to user mode after an interrupt: pending signals */
void uproc_irq_return(struct trapframe *tf)
{
    struct task *t = task_current();
    if (t) uproc_signals(t, tf);
}
