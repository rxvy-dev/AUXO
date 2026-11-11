/* The Linux x86_64 system-call interface. Numbers, structures and errno values are Linux's,
 * so unmodified programs (glibc, musl, BusyBox, GCC …) run. */
#include "kern.h"
#include "arch.h"
#include "rs.h"
#include "fs.h"
#include "uproc.h"
#include "auxo.h"

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
       EOPNOTSUPP = 95 };

#define AT_FDCWD -100

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
static int at_dir(struct task *t, long dirfd, const char *path, struct node **dir)
{
    if (path[0] == '/' || dirfd == AT_FDCWD) { *dir = t->cwd; return 0; }
    struct file *f = getf(t, dirfd);
    if (!f || !f->node) return -EBADF;
    if (f->node->type != N_DIR) return -ENOTDIR;
    *dir = f->node;
    return 0;
}

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

/* generated /proc files: an unlinked node holding the text */
static struct node *proc_file(struct task *t, const char *path)
{
    static char buf[16384];
    int n = 0;
    if (!strcmp(path, "/proc/self/maps") || !strcmp(path, "/proc/thread-self/maps")) {
        for (int i = 0; i < t->mm->nv && n < (int)sizeof buf - 200; i++) {
            struct vma *v = &t->mm->v[i];
            n += ksnprintf(buf + n, sizeof buf - (size_t)n, "%lx-%lx %c%c%cp %08lx 08:01 %u %s\n", v->start, v->end,
                           v->prot & 1 ? 'r' : '-', v->prot & 2 ? 'w' : '-', v->prot & 4 ? 'x' : '-', v->off,
                           v->file ? v->file->ino : 0, v->start >= 0x7FFFFF000000ULL ? "[stack]" : "");
        }
    } else if (!strcmp(path, "/proc/meminfo")) {
        n = ksnprintf(buf, sizeof buf, "MemTotal: %lu kB\nMemFree: %lu kB\nMemAvailable: %lu kB\n",
                      (unsigned long)(pmm_total_pages() * 4), (unsigned long)(pmm_free_pages() * 4), (unsigned long)(pmm_free_pages() * 4));
    } else if (!strcmp(path, "/proc/cpuinfo")) {
        n = ksnprintf(buf, sizeof buf, "processor\t: 0\nvendor_id\t: GenuineIntel\nmodel name\t: %s\nflags\t\t: fpu sse sse2\n\n", cpu_brand());
    } else if (!strcmp(path, "/proc/self/stat")) {
        n = ksnprintf(buf, sizeof buf, "%d (%s) R %d %d %d 0 -1 0 0 0 0 0 0 0 0 0 20 0 1 0 0 0 0\n", t->tgid, t->name, t->ppid, t->pgid, t->pgid);
    } else if (!strcmp(path, "/proc/version")) {
        n = ksnprintf(buf, sizeof buf, "Linux version 6.8.0-auxobsd (AuxoBSD " AUXO_VERSION ")\n");
    } else if (!strcmp(path, "/proc/filesystems")) {
        n = ksnprintf(buf, sizeof buf, "\ttmpfs\n");
    } else return 0;
    struct node *nd = rs_kmalloc(sizeof *nd);
    if (!nd) return 0;
    memset(nd, 0, sizeof *nd);
    nd->name = rs_kmalloc(8);
    if (nd->name) strlcpy(nd->name, "proc", 8);
    nd->type = N_REG; nd->mode = 0444; nd->unlinked = 1; nd->nlink = 0;
    fs_write(nd, 0, buf, (size_t)n);
    return nd;
}

/* ── open / close / read / write ────────────────────────────────────── */
static long sys_openat(struct task *t, long dirfd, uint64_t up, long flags, long mode)
{
    char path[1024];
    long r = upath(t, up, path, sizeof path);
    if (r < 0) return r;
    struct node *dir, *n;
    if (!strncmp(path, "/proc/", 6)) {
        struct node *pn = proc_file(t, path);
        if (pn) {
            struct file *f = file_new(0, pn, 0);
            pn->refs--;                                     /* file_new took one; the file owns it */
            if (!f) return -ENOMEM;
            strlcpy(f->path, path, sizeof f->path);
            int fd = alloc_fd(t, f, 0, (flags & 02000000) != 0);
            if (fd < 0) file_put(f);
            return fd;
        }
    }
    if ((r = at_dir(t, dirfd, path, &dir)) < 0) return r;
    r = fs_lookup(dir, path, !(flags & 0400000), &n);
    if (r == -ENOENT && (flags & 0100)) {                  /* O_CREAT */
        struct node *pd; char name[256];
        if ((r = fs_lookup_parent(dir, path, &pd, name, sizeof name)) < 0) return r;
        n = fs_create(pd, name, N_REG, (uint16_t)(mode & ~t->umask & 07777));
        if (!n) return -ENOMEM;
        r = 0;
    } else if (r == 0 && (flags & 0100) && (flags & 0200)) return -EEXIST;
    if (r < 0) return r;
    if ((flags & 0200000) && n->type != N_DIR) return -ENOTDIR;
    if (n->type == N_DIR && (flags & 3)) return -EISDIR;
    if (n->type == N_LNK && (flags & 0400000) && !(flags & 010000000)) return -ELOOP;
    if ((flags & 01000) && n->type == N_REG && (flags & 3)) fs_truncate(n, 0);
    struct file *f = file_new(0, n, (int)flags);
    if (!f) return -ENOMEM;
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
    if (n > KBUF) n = KBUF;
    long r = file_read(t, f, kb, n);
    if (r > 0 && copyout(t->mm, ubuf, kb, (size_t)r) < 0) r = -EFAULT;
    rs_kfree(kb);
    return r;
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
    for (;;) {
        const char *name; uint64_t ino; uint8_t type;
        if (idx == 0) { name = "."; ino = f->node->ino; type = 4; }
        else if (idx == 1) { name = ".."; ino = f->node->parent->ino; type = 4; }
        else {
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
        if ((r = at_dir(t, dirfd, path, &dir)) < 0) return r;
        if ((r = fs_lookup(dir, path, !(flags & 0x100), &n)) < 0) return r;
    }
    return x ? do_statx(t, n, ubuf) : do_stat_node(t, n, ubuf);
}

static long sys_readlinkat(struct task *t, long dirfd, uint64_t up, uint64_t ubuf, uint64_t sz)
{
    char path[1024], out[512];
    long r;
    if ((r = upath(t, up, path, sizeof path)) < 0) return r;
    if (!strcmp(path, "/proc/self/exe")) strlcpy(out, t->exe, sizeof out);
    else if (!strncmp(path, "/proc/self/fd/", 14)) {
        struct file *f = getf(t, katoi(path + 14));
        if (!f) return -ENOENT;
        if (f->kind) ksnprintf(out, sizeof out, "pipe:[%d]", katoi(path + 14));
        else strlcpy(out, f->path, sizeof out);
    } else {
        struct node *dir, *n;
        if ((r = at_dir(t, dirfd, path, &dir)) < 0) return r;
        if ((r = fs_lookup(dir, path, 0, &n)) < 0) return r;
        if (n->type != N_LNK) return -EINVAL;
        size_t l = n->size < sizeof out - 1 ? (size_t)n->size : sizeof out - 1;
        memcpy(out, n->data, l); out[l] = 0;
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
    struct node *dir, *n;
    if ((r = upath(t, up, path, sizeof path)) < 0) return r;
    if ((r = at_dir(t, dirfd, path, &dir)) < 0) return r;
    if ((r = fs_lookup(dir, path, 1, &n)) < 0) return r;
    if ((mode & 1) && n->type == N_REG && !(n->mode & 0111)) return -EACCES;
    return 0;
}

static long sys_mkdirat(struct task *t, long dirfd, uint64_t up, long mode)
{
    char path[1024], name[256];
    long r;
    struct node *dir, *pd;
    if ((r = upath(t, up, path, sizeof path)) < 0) return r;
    if ((r = at_dir(t, dirfd, path, &dir)) < 0) return r;
    if ((r = fs_lookup_parent(dir, path, &pd, name, sizeof name)) < 0) return r;
    if (fs_child(pd, name)) return -EEXIST;
    return fs_create(pd, name, N_DIR, (uint16_t)(mode & ~t->umask & 07777)) ? 0 : -ENOMEM;
}

static long sys_unlinkat(struct task *t, long dirfd, uint64_t up, long flags)
{
    char path[1024], name[256];
    long r;
    struct node *dir, *pd;
    if ((r = upath(t, up, path, sizeof path)) < 0) return r;
    if ((r = at_dir(t, dirfd, path, &dir)) < 0) return r;
    if ((r = fs_lookup_parent(dir, path, &pd, name, sizeof name)) < 0) return r;
    return fs_unlink(pd, name, (flags & 0x200) != 0);
}

static long sys_renameat(struct task *t, long ofd, uint64_t uo, long nfd, uint64_t un)
{
    char op[1024], np[1024], on[256], nn[256];
    long r;
    struct node *od, *nd, *opd, *npd;
    if ((r = upath(t, uo, op, sizeof op)) < 0 || (r = upath(t, un, np, sizeof np)) < 0) return r;
    if ((r = at_dir(t, ofd, op, &od)) < 0 || (r = at_dir(t, nfd, np, &nd)) < 0) return r;
    if ((r = fs_lookup_parent(od, op, &opd, on, sizeof on)) < 0) return r;
    if ((r = fs_lookup_parent(nd, np, &npd, nn, sizeof nn)) < 0) return r;
    return fs_rename(opd, on, npd, nn);
}

static long sys_symlinkat(struct task *t, uint64_t ut, long dirfd, uint64_t up)
{
    char target[1024], path[1024], name[256];
    long r;
    struct node *dir, *pd;
    if ((r = upath(t, ut, target, sizeof target)) < 0 || (r = upath(t, up, path, sizeof path)) < 0) return r;
    if ((r = at_dir(t, dirfd, path, &dir)) < 0) return r;
    if ((r = fs_lookup_parent(dir, path, &pd, name, sizeof name)) < 0) return r;
    if (fs_child(pd, name)) return -EEXIST;
    struct node *n = fs_create(pd, name, N_LNK, 0777);
    if (!n) return -ENOMEM;
    return fs_write(n, 0, target, strlen(target)) < 0 ? -ENOMEM : 0;
}

static long sys_linkat(struct task *t, long ofd, uint64_t uo, long nfd, uint64_t un)
{
    char op[1024], np[1024], nn[256];
    long r;
    struct node *od, *nd, *src, *npd;
    if ((r = upath(t, uo, op, sizeof op)) < 0 || (r = upath(t, un, np, sizeof np)) < 0) return r;
    if ((r = at_dir(t, ofd, op, &od)) < 0 || (r = at_dir(t, nfd, np, &nd)) < 0) return r;
    if ((r = fs_lookup(od, op, 0, &src)) < 0) return r;
    if ((r = fs_lookup_parent(nd, np, &npd, nn, sizeof nn)) < 0) return r;
    return fs_link(src, npd, nn);
}

static long sys_chdir(struct task *t, uint64_t up)
{
    char path[1024];
    long r;
    struct node *n;
    if ((r = upath(t, up, path, sizeof path)) < 0) return r;
    if ((r = fs_lookup(t->cwd, path, 1, &n)) < 0) return r;
    if (n->type != N_DIR) return -ENOTDIR;
    t->cwd = n;
    return 0;
}

static long sys_getcwd(struct task *t, uint64_t ubuf, uint64_t sz)
{
    char p[1024];
    fs_path(t->cwd, p, sizeof p);
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
struct child_start { struct task *t; struct trapframe tf; uint64_t fs; };

static void child_entry(uintptr_t arg)
{
    struct child_start *cs = (struct child_start *)arg;
    struct task *t = cs->t;
    struct trapframe tf = cs->tf;
    t->pid = rs_current_pid();
    if (!t->is_thread) t->tgid = t->pid;
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
    cs->t = c;
    cs->tf = *t->tf;
    cs->tf.rax = 0;
    if (stack) cs->tf.rsp = stack;
    cs->fs = (flags & 0x80000) ? tls : rdmsr(0xC0000100);
    if (flags & 0x01000000) c->clear_child_tid = ctid;                     /* CLONE_CHILD_CLEARTID */
    int pid = rs_spawn(c->name, child_entry, (uintptr_t)cs);
    if (pid < 0) { rs_kfree(cs); task_free(c); return -EAGAIN; }
    c->pid = pid;
    if (!thread) c->tgid = pid;
    if (flags & 0x100000) { int p32 = pid; copyout(t->mm, ptid, &p32, 4); }  /* CLONE_PARENT_SETTID */
    if (flags & 0x200000) { int p32 = pid; copyout(c->mm, ctid, &p32, 4); }  /* CLONE_CHILD_SETTID */
    if (vfork) { while (!c->vfork_done && c->used && c->state != 2) wait_tick(); }
    return pid;
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
    close_files(t);
    if (t->mm) { struct mm *m = t->mm; t->mm = 0; mm_put(m); }
    rs_set_ucr3(0);
    if (t->is_thread) { task_free(t); rs_thread_exit(); }
    /* children of this process are now nobody's */
    for (int i = 0; i < 128; i++) { struct task *c = task_slot(i); if (c->used && c->ppid == t->tgid && c != t) c->ppid = 0; }
    t->exit_status = status;
    t->state = 2;                                           /* zombie: the parent's wait4 reaps it */
    /* nobody will wait for orphans: reap now */
    if (t->ppid && !task_by_pid(t->ppid)) task_free(t);
    rs_thread_exit();
}

static void exit_group(struct task *t, int status)
{
    for (int i = 0; i < 128; i++) {
        struct task *o = task_slot(i);
        if (o->used && o != t && o->tgid == t->tgid && o->state != 2) o->pending |= 1ULL << 8;   /* SIGKILL */
    }
    task_exit(t, status);
}

static long sys_wait4(struct task *t, long pid, uint64_t ustatus, long options)
{
    for (;;) {
        int have = 0;
        for (int i = 0; i < 128; i++) {
            struct task *c = task_slot(i);
            if (!c->used || c->is_thread || c->ppid != t->tgid) continue;
            if (pid > 0 && c->pid != pid) continue;
            if (pid == 0 && c->pgid != t->pgid) continue;
            if (pid < -1 && c->pgid != -pid) continue;
            have = 1;
            if (c->state == 2) {
                int st = c->exit_status, cp = c->pid;
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
#define NFUTEX 64
static struct { struct mm *mm; uint64_t addr; int pid; volatile int woken; } fw[NFUTEX];

static void futex_wake_addr(struct mm *m, uint64_t addr, int n)
{
    for (int i = 0; i < NFUTEX && n > 0; i++)
        if (fw[i].pid && fw[i].mm == m && fw[i].addr == addr && !fw[i].woken) { fw[i].woken = 1; n--; }
}

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
        if (copyin(t->mm, &cur, addr, 4) < 0) return -EFAULT;
        if (cur != (uint32_t)val) return -EAGAIN;
        uint64_t until = ~0ULL;
        if (utimeout) {
            int64_t ts[2];
            if (copyin(t->mm, ts, utimeout, 16) < 0) return -EFAULT;
            uint64_t ticks = (uint64_t)ts[0] * AUXO_HZ + (uint64_t)ts[1] / (1000000000 / AUXO_HZ);
            until = cmd == 9 ? ticks : kernel_ticks() + ticks;  /* BITSET timeouts are absolute (CLOCK_MONOTONIC) */
        }
        int slot = -1;
        uint64_t f;
        __asm__ volatile("pushfq; pop %0; cli" : "=r"(f) :: "memory");
        for (int i = 0; i < NFUTEX; i++) if (!fw[i].pid) { slot = i; fw[i].pid = t->pid; fw[i].mm = t->mm; fw[i].addr = addr; fw[i].woken = 0; break; }
        if (f & 0x200) __asm__ volatile("sti");
        if (slot < 0) { wait_tick(); return 0; }
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
    if (f->kind == 1) { if (pipe_readable(f)) r |= 1; }
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
static uint32_t boot_epoch;

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

static long sys_kill(struct task *t, long pid, long sig)
{
    if (sig < 0 || sig > 64) return -EINVAL;
    int found = 0;
    for (int i = 0; i < 128; i++) {
        struct task *o = task_slot(i);
        if (!o->used || o->state == 2) continue;
        int match = pid > 0 ? o->tgid == pid && !o->is_thread : pid == 0 ? o->pgid == t->pgid : pid == -1 ? 1 : o->pgid == -pid;
        if (!match) continue;
        found = 1;
        if (sig) o->pending |= 1ULL << (sig - 1);
    }
    return found ? 0 : -ESRCH;
}

/* ── misc ───────────────────────────────────────────────────────────── */
static long sys_uname(struct task *t, uint64_t u)
{
    char b[390];
    memset(b, 0, sizeof b);
    strlcpy(b, "Linux", 65);
    strlcpy(b + 65, auxo.hostname, 65);
    strlcpy(b + 130, "6.8.0-auxobsd", 65);
    strlcpy(b + 195, "#1 AuxoBSD " AUXO_VERSION, 65);
    strlcpy(b + 260, "x86_64", 65);
    strlcpy(b + 325, "auxolinux.com", 65);
    return copyout(t->mm, u, b, sizeof b);
}

static long sys_ioctl(struct task *t, long fd, uint64_t req, uint64_t arg)
{
    struct file *f = getf(t, fd);
    if (!f) return -EBADF;
    int tty = f->node && f->node->type == N_CHR && f->node->dev == DEV_TTY;
    if (req == 0x5421) { if (arg) { int v; if (copyin(t->mm, &v, arg, 4) == 0) f->flags = v ? f->flags | 04000 : f->flags & ~04000; } return 0; }
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
    case 5: case 6: case 7: case 36: case 37: case 38: return 0;   /* locks: always granted */
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
    case 4: return sys_fstatat(t, AT_FDCWD, a, b, 0, 0);
    case 5: { char z = 0; (void)z; struct file *fl = getf(t, (long)a); if (!fl) return -EBADF;
              if (!fl->node) { struct node tmp; memset(&tmp, 0, sizeof tmp); tmp.type = N_FIFO; tmp.mode = 0600; tmp.ino = 77; return do_stat_node(t, &tmp, b); }
              return do_stat_node(t, fl->node, b); }
    case 6: return sys_fstatat(t, AT_FDCWD, a, b, 0x100, 0);
    case 7: return sys_poll(t, a, (long)b, (int64_t)(int)c);
    case 8: return sys_lseek(t, (long)a, (int64_t)b, (long)c);
    case 9: return sys_mmap(t, a, b, (long)c, (long)d, (long)(int)e, f);
    case 10: return mm_protect(t->mm, a, b, (uint32_t)(c & 7));
    case 11: return mm_unmap(t->mm, a, b);
    case 12: return sys_brk(t, a);
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
    case 25: return sys_mremap(t, a, b, c, (long)d);
    case 26: case 27: case 28: return 0;                       /* msync, mincore, madvise */
    case 32: { struct file *fl = getf(t, (long)a); if (!fl) return -EBADF; int n = alloc_fd(t, fl, 0, 0); if (n >= 0) fl->refs++; return n; }
    case 33: return sys_dup3(t, (long)a, (long)b, 0);
    case 35: return sys_nanosleep(t, a);
    case 39: return t->tgid;
    case 41: return -EAFNOSUPPORT;                             /* socket: not yet */
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
    case 76: { char p[1024]; struct node *n; long r = upath(t, a, p, sizeof p); if (r < 0) return r;
               if ((r = fs_lookup(t->cwd, p, 1, &n)) < 0) return r;
               return sys_truncate(n, (int64_t)b); }
    case 77: { struct file *fl = getf(t, (long)a); if (!fl || !fl->node) return -EBADF; return sys_truncate(fl->node, (int64_t)b); }
    case 79: return sys_getcwd(t, a, b);
    case 80: return sys_chdir(t, a);
    case 81: { struct file *fl = getf(t, (long)a); if (!fl || !fl->node) return -EBADF; if (fl->node->type != N_DIR) return -ENOTDIR; t->cwd = fl->node; return 0; }
    case 82: return sys_renameat(t, AT_FDCWD, a, AT_FDCWD, b);
    case 83: return sys_mkdirat(t, AT_FDCWD, a, (long)b);
    case 84: return sys_unlinkat(t, AT_FDCWD, a, 0x200);
    case 86: return sys_linkat(t, AT_FDCWD, a, AT_FDCWD, b);
    case 87: return sys_unlinkat(t, AT_FDCWD, a, 0);
    case 88: return sys_symlinkat(t, a, AT_FDCWD, b);
    case 89: return sys_readlinkat(t, AT_FDCWD, a, b, c);
    case 90: { char p[1024]; struct node *n; long r = upath(t, a, p, sizeof p); if (r < 0) return r;
               if ((r = fs_lookup(t->cwd, p, 1, &n)) < 0) return r;
               n->mode = (uint16_t)(b & 07777); return 0; }
    case 91: { struct file *fl = getf(t, (long)a); if (!fl || !fl->node) return -EBADF; fl->node->mode = (uint16_t)(b & 07777); return 0; }
    case 268: { char p[1024]; struct node *dir, *n; long r = upath(t, b, p, sizeof p); if (r < 0) return r;
                if ((r = at_dir(t, (long)(int)a, p, &dir)) < 0) return r;
                if ((r = fs_lookup(dir, p, 1, &n)) < 0) return r;
                n->mode = (uint16_t)(c & 07777); return 0; }
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
    case 109: { struct task *o = a ? task_by_pid((int)a) : t; if (o) o->pgid = b ? (int)b : o->tgid; return 0; }
    case 110: return t->ppid ? t->ppid : 1;
    case 111: return t->pgid;
    case 112: t->pgid = t->tgid; return t->tgid;
    case 115: return 0;                                        /* getgroups */
    case 121: { struct task *o = a ? task_by_pid((int)a) : t; return o ? o->pgid : -ESRCH; }
    case 124: return t->tgid;
    case 127: { uint64_t z = t->pending; return copyout(t->mm, a, &z, 8); }
    case 130: { uint64_t m; if (copyin(t->mm, &m, a, 8) < 0) return -EFAULT; uint64_t old = t->sigmask; t->sigmask = m;
                while (!(t->pending & ~t->sigmask)) wait_tick();
                t->sigmask = old;
                return -EINTR; }
    case 131:
        if (b) { uint8_t ss[24]; memset(ss, 0, sizeof ss); int fl = 2; memcpy(ss + 8, &fl, 4); copyout(t->mm, b, ss, 24); }
        return 0;
    case 137: case 138: { uint64_t sf[15]; memset(sf, 0, sizeof sf); sf[0] = 0x01021994; sf[1] = 4096; sf[2] = sf[3] = sf[4] = pmm_free_pages(); sf[5] = sf[6] = 100000; sf[8] = 255; sf[9] = 4096; return copyout(t->mm, b, sf, 120); }
    case 157: return 0;                                        /* prctl */
    case 158:                                                  /* arch_prctl */
        if (a == 0x1002) { wrmsr(0xC0000100, b); return 0; }
        if (a == 0x1003) { uint64_t v = rdmsr(0xC0000100); return copyout(t->mm, b, &v, 8); }
        if (a == 0x3001) return -EINVAL;                       /* CET */
        return -EINVAL;
    case 160: return 0;                                        /* setrlimit */
    case 186: return t->pid;
    case 200: return sys_kill(t, (long)a, (long)b);
    case 201: { int64_t s, ns; now_ts(0, &s, &ns); if (a) copyout(t->mm, a, &s, 8); return s; }
    case 202: return sys_futex(t, a, (long)b, c, d);
    case 203: return 0;
    case 204: { uint64_t mask = 1; if (b < 8) return -EINVAL; return copyout(t->mm, c, &mask, 8) < 0 ? -EFAULT : 8; }
    case 213: case 291: return -ENOSYS;                        /* epoll: not yet */
    case 217: return sys_getdents64(t, (long)a, b, c);
    case 218: t->clear_child_tid = a; return t->pid;
    case 228: { int64_t ts[2]; now_ts((int)a, &ts[0], &ts[1]); return copyout(t->mm, b, ts, 16); }
    case 229: { int64_t ts[2] = { 0, 10000000 }; return b ? copyout(t->mm, b, ts, 16) : 0; }
    case 230: { int64_t ts[2]; if (copyin(t->mm, ts, c, 16) < 0) return -EFAULT;
                if (b & 1) { int64_t s, ns; now_ts((int)a, &s, &ns); ts[0] -= s; ts[1] -= ns; if (ts[1] < 0) { ts[0]--; ts[1] += 1000000000; } if (ts[0] < 0) return 0; }
                uint64_t until = kernel_ticks() + (uint64_t)ts[0] * AUXO_HZ + (uint64_t)ts[1] / 10000000;
                while (kernel_ticks() < until) { if (t->pending & ~t->sigmask) return -EINTR; rs_sleep(until - kernel_ticks()); }
                return 0; }
    case 231: exit_group(t, (int)(a & 0xFF) << 8);
    case 232: case 233: case 281: return -ENOSYS;
    case 234: return sys_kill(t, (long)b, (long)c);
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
    case 280: return 0;                                        /* utimensat */
    case 284: case 290: return -ENOSYS;                        /* eventfd: not yet */
    case 292: return sys_dup3(t, (long)a, (long)b, (long)c);
    case 293: return sys_pipe2(t, a, (long)b);
    case 302: return sys_prlimit(t, (long)b, c, d);
    case 318: return sys_getrandom(t, a, b);
    case 319: return sys_memfd(t, a, (long)b);
    case 324: return 0;                                        /* membarrier */
    case 332: { char p[1024]; long r; struct node *dir, *n;
                if ((r = upath(t, b, p, sizeof p)) < 0) return r;
                if (!p[0] && (c & 0x1000)) { struct file *fl = getf(t, (long)(int)a); if (!fl) return -EBADF;
                    if (!fl->node) { struct node tmp; memset(&tmp, 0, sizeof tmp); tmp.type = N_FIFO; tmp.mode = 0600; return do_statx(t, &tmp, e); }
                    return do_statx(t, fl->node, e); }
                if ((r = at_dir(t, (long)(int)a, p, &dir)) < 0) return r;
                if ((r = fs_lookup(dir, p, !(c & 0x100), &n)) < 0) return r;
                return do_statx(t, n, e); }
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
    long r = syscall_body(t, tf);
    if (sys_trace) kprintf("[%d %s] %lu(%lx, %lx, %lx%s%s%s) = %ld\n", t->pid, t->name, nr, a0, a1, a2, s0[0] ? ", \"" : "", s0, s0[0] ? "\"" : "", r);
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
