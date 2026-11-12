/* The in-memory filesystem tree (see fs.h). */
#include "kern.h"
#include "arch.h"
#include "rs.h"
#include "fs.h"
#include "fdobj.h"
#include "cred.h"

size_t strspn_slash(const char *s);
static struct node *root;
static uint32_t next_ino = 1, generation;
static volatile int lock_owner = -1, lock_depth;
static int mounting;                       /* nodes from a boot image aren't "changed" */

void fs_lock(void)
{
    int me = rs_current_pid();
    for (;;) {
        uint64_t f;
        __asm__ volatile("pushfq; pop %0; cli" : "=r"(f) :: "memory");
        if (lock_owner == -1 || lock_owner == me) { lock_owner = me; lock_depth++; if (f & 0x200) __asm__ volatile("sti"); return; }
        if (f & 0x200) __asm__ volatile("sti");
        rs_yield();
    }
}

void fs_unlock(void)
{
    uint64_t f;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(f) :: "memory");
    if (--lock_depth == 0) lock_owner = -1;
    if (f & 0x200) __asm__ volatile("sti");
}

uint32_t fs_generation(void) { return generation; }

void fs_touch(struct node *n) { fs_lock(); generation++; n->changed = 1; n->wgen = generation; if (n->parent && n->parent != n) inotify_event(n->parent, n, n->name, IN_ATTRIB, 0); fs_unlock(); }

static void touch_tree(struct node *n)
{
    n->changed = 1;
    n->wgen = generation;
    if (n->type == N_DIR) for (struct node *c = n->child; c; c = c->next) touch_tree(c);
}

uint32_t fs_now(void)
{
    struct rtc_time t;
    rtc_read(&t);
    /* seconds since 1970 (good enough for file times) */
    int y = t.year, m = t.month;
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + t.day - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = (long)era * 146097 + (long)doe - 719468;
    return (uint32_t)(days * 86400 + t.hour * 3600 + t.minute * 60 + t.second);
}

static char *kstrdup(const char *s, size_t n)
{
    char *p = rs_kmalloc(n + 1);
    if (!p) return 0;
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

static struct node *new_node(struct node *dir, const char *name, size_t nlen, int type, uint16_t mode)
{
    struct node *n = rs_kmalloc(sizeof *n);
    if (!n) return 0;
    memset(n, 0, sizeof *n);
    n->name = kstrdup(name, nlen);
    if (!n->name) { rs_kfree(n); return 0; }
    n->type = (uint8_t)type;
    n->mode = mode;
    n->ino = next_ino++;
    n->nlink = 1;
    n->mtime = 0;
    n->parent = dir ? dir : n;
    if (dir) { n->next = dir->child; dir->child = n; }
    generation++;
    n->changed = !mounting;
    n->wgen = generation;
    if (!mounting) cred_new_node(dir, n);
    return n;
}

struct node *fs_root(void) { return root; }

struct node *fs_child(struct node *dir, const char *name)
{
    if (!dir || dir->type != N_DIR) return 0;
    for (struct node *c = dir->child; c; c = c->next) if (!strcmp(c->name, name)) return c;
    return 0;
}

static struct node *child_n(struct node *dir, const char *name, size_t n)
{
    for (struct node *c = dir->child; c; c = c->next) if (!strncmp(c->name, name, n) && !c->name[n]) return c;
    return 0;
}

struct node *fs_child_n(struct node *dir, const char *name, size_t n) { return dir && dir->type == N_DIR ? child_n(dir, name, n) : 0; }

/* a directory of its own, in no tree: the root of a tmpfs */
struct node *fs_new_root(uint16_t mode)
{
    fs_lock();
    struct node *n = new_node(0, "", 0, N_DIR, mode);
    if (n) { n->changed = 0; n->mtime = fs_now(); }
    fs_unlock();
    return n;
}

struct node *fs_create(struct node *dir, const char *name, int type, uint16_t mode)
{
    fs_lock();
    struct node *n = fs_child(dir, name);
    if (!n) { n = new_node(dir, name, strlen(name), type, mode); if (n) { n->mtime = fs_now(); inotify_event(dir, n, name, IN_CREATE, 0); } }
    fs_unlock();
    return n;
}

/* walk: resolve `path` from `cwd`; depth counts symlinks followed */
static int walk(struct node *cwd, const char *path, int follow, struct node **out, int depth)
{
    if (depth > 40) return -40;                              /* ELOOP */
    struct node *cur = path[0] == '/' ? root : (cwd ? cwd : root);
    const char *p = path;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *e = p;
        while (*e && *e != '/') e++;
        size_t l = (size_t)(e - p);
        int last = !*e || (e[0] == '/' && !e[strspn_slash(e)]);
        if (cur->type != N_DIR) return -20;                  /* ENOTDIR */
        struct node *nx;
        if (l == 1 && p[0] == '.') nx = cur;
        else if (l == 2 && p[0] == '.' && p[1] == '.') nx = cur->parent;
        else nx = child_n(cur, p, l);
        if (!nx) return -2;                                  /* ENOENT */
        if (nx->type == N_LNK && (!last || follow)) {
            char target[512];
            size_t tl = nx->size < sizeof target - 1 ? (size_t)nx->size : sizeof target - 1;
            memcpy(target, nx->data, tl);
            target[tl] = 0;
            struct node *t;
            int r = walk(cur, target, 1, &t, depth + 1);
            if (r < 0) return r;
            nx = t;
        }
        cur = nx;
        p = e;
    }
    *out = cur;
    return 0;
}

size_t strspn_slash(const char *s) { size_t i = 0; while (s[i] == '/') i++; return i; }

int fs_lookup(struct node *cwd, const char *path, int follow, struct node **out)
{
    if (!path[0]) return -2;
    fs_lock();
    int r = walk(cwd, path, follow, out, 0);
    fs_unlock();
    return r;
}

int fs_lookup_parent(struct node *cwd, const char *path, struct node **dir, char *name, size_t n)
{
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/') len--;
    size_t s = len;
    while (s > 0 && path[s - 1] != '/') s--;
    if (len - s >= n || len == s) return len == s && len ? -21 : -36;      /* "/" itself, or too long */
    memcpy(name, path + s, len - s);
    name[len - s] = 0;
    char dpath[1024];
    if (s == 0) { *dir = cwd ? cwd : root; }
    else {
        if (s >= sizeof dpath) return -36;
        memcpy(dpath, path, s);
        dpath[s] = 0;
        int r = fs_lookup(cwd, dpath, 1, dir);
        if (r < 0) return r;
    }
    if ((*dir)->type != N_DIR) return -20;
    return 0;
}

static void free_data(struct node *n)
{
    if (n->cap && !n->maps) for (uint64_t o = 0; o < n->cap; o += PAGE_SIZE) pmm_free_page((uint64_t)(uintptr_t)n->data + o);
    n->data = 0; n->cap = 0; n->size = 0;
}

void fs_put(struct node *n)
{
    if (!n) return;
    fs_lock();
    if (--n->refs <= 0 && n->unlinked && n->nlink == 0) {
        free_data(n);
        rs_kfree(n->name);
        rs_kfree(n);
    }
    fs_unlock();
}

static void detach(struct node *n)
{
    struct node *d = n->parent;
    struct node **pp = &d->child;
    while (*pp && *pp != n) pp = &(*pp)->next;
    if (*pp) *pp = n->next;
    n->next = 0;
}

int fs_unlink(struct node *dir, const char *name, int rmdir)
{
    fs_lock();
    struct node *n = fs_child(dir, name);
    int r = 0;
    if (!n) r = -2;
    else if (rmdir && n->type != N_DIR) r = -20;
    else if (!rmdir && n->type == N_DIR) r = -21;
    else if (rmdir && n->child) r = -39;                    /* ENOTEMPTY */
    else {
        detach(n);
        n->unlinked = 1;
        if (n->nlink) n->nlink--;
        generation++;
        inotify_event(dir, n, name, IN_DELETE, 0);
        if (n->nlink == 0) inotify_event(0, n, 0, IN_DELETE_SELF, 0);
        if (n->refs <= 0 && n->nlink == 0) {
            if (n->cap) free_data(n);
            rs_kfree(n->name);
            rs_kfree(n);
        }
    }
    fs_unlock();
    return r;
}

int fs_rename(struct node *odir, const char *oname, struct node *ndir, const char *nname)
{
    fs_lock();
    struct node *n = fs_child(odir, oname);
    if (!n) { fs_unlock(); return -2; }
    struct node *old = fs_child(ndir, nname);
    if (old == n) { fs_unlock(); return 0; }
    if (old) {
        if (old->type == N_DIR && old->child) { fs_unlock(); return -39; }
        detach(old);
        old->unlinked = 1;
        if (old->nlink) old->nlink--;
        if (old->refs <= 0 && old->nlink == 0) { free_data(old); rs_kfree(old->name); rs_kfree(old); }
    }
    uint32_t cookie = inotify_cookie();
    inotify_event(odir, n, oname, IN_MOVED_FROM, cookie);
    detach(n);
    char *nm = kstrdup(nname, strlen(nname));
    if (nm) { rs_kfree(n->name); n->name = nm; }
    n->parent = ndir;
    n->next = ndir->child;
    ndir->child = n;
    generation++;
    inotify_event(ndir, n, nname, IN_MOVED_TO, cookie);
    inotify_event(0, n, 0, IN_MOVE_SELF, 0);
    touch_tree(n);                         /* it lives somewhere new: keep it there */
    fs_unlock();
    return 0;
}

int fs_link(struct node *target, struct node *dir, const char *name)
{
    /* hard links: a new node sharing the contents (good enough: the rootfs uses a few) */
    fs_lock();
    if (fs_child(dir, name)) { fs_unlock(); return -17; }
    struct node *n = new_node(dir, name, strlen(name), target->type, target->mode);
    if (!n) { fs_unlock(); return -12; }
    n->data = target->data; n->size = target->size; n->cap = 0;
    if (target->cap) {                                           /* owned data: copy it */
        n->data = 0;
        fs_unlock();
        if (fs_write(n, 0, target->data, target->size) < 0) return -12;
        return 0;
    }
    fs_unlock();
    return 0;
}

int fs_reserve(struct node *n, uint64_t want)
{
    if (n->cap >= want && n->cap) return 0;
    uint64_t cap = n->cap ? n->cap : PAGE_SIZE;
    while (cap < want) cap *= 2;
    if (cap < PAGE_SIZE) cap = PAGE_SIZE;
    /* our own pages, nothing mapped shared: grow in place when the pages after are free (no copy,
     * and no holes left behind, which big files would otherwise run into) */
    if (n->cap && !n->maps) {
        uint64_t exact = (want + 0x1FFFFF) & ~0x1FFFFFULL;      /* or just what's needed, in 2 MiB */
        uint64_t tries[2] = { cap, exact < cap ? exact : cap };
        for (int k = 0; k < 2; k++)
            if (pmm_grow((uint64_t)(uintptr_t)n->data, n->cap / PAGE_SIZE, tries[k] / PAGE_SIZE) == 0) {
                memset(n->data + n->cap, 0, tries[k] - n->cap);
                n->cap = tries[k];
                return 0;
            }
    }
    uint64_t pg = pmm_alloc_contig_any(cap / PAGE_SIZE);
    if (!pg && want > PAGE_SIZE) { cap = (want + 0x1FFFFF) & ~0x1FFFFFULL; pg = pmm_alloc_contig_any(cap / PAGE_SIZE); }
    if (!pg) return -12;
    uint8_t *d = (uint8_t *)(uintptr_t)pg;
    if (n->size) memcpy(d, n->data, n->size);
    memset(d + n->size, 0, cap - n->size);
    /* pages a program has mapped shared stay where they are (leaked, rather than reused under it) */
    if (n->cap && !n->maps) for (uint64_t o = 0; o < n->cap; o += PAGE_SIZE) pmm_free_page((uint64_t)(uintptr_t)n->data + o);
    n->data = d;
    n->cap = cap;
    return 0;
}

long fs_read(struct node *n, uint64_t off, void *buf, size_t len)
{
    if (n->type == N_DIR) return -21;
    if (off >= n->size) return 0;
    if (len > n->size - off) len = n->size - off;
    memcpy(buf, n->data + off, len);
    return (long)len;
}

long fs_write(struct node *n, uint64_t off, const void *buf, size_t len)
{
    fs_lock();
    if (n->type == N_DIR) { fs_unlock(); return -21; }
    uint64_t end = off + len;
    if (!n->cap || end > n->cap) {
        if (fs_reserve(n, end > n->size ? end : n->size) < 0) { fs_unlock(); return -28; }   /* ENOSPC */
    }
    if (off > n->size) memset(n->data + n->size, 0, off - n->size);
    memcpy(n->data + off, buf, len);
    if (end > n->size) n->size = end;
    n->mtime = fs_now();
    generation++;
    n->changed = 1; n->wgen = generation;
    if (n->parent != n) inotify_event(n->parent, n, n->name, IN_MODIFY, 0);
    fs_unlock();
    return (long)len;
}

int fs_truncate(struct node *n, uint64_t len)
{
    fs_lock();
    if (len > n->size) {
        if (fs_reserve(n, len) < 0) { fs_unlock(); return -28; }
        memset(n->data + n->size, 0, len - n->size);
    } else if (len < n->size && !n->cap) {
        /* borrowed data: just shorten the view */
    }
    n->size = len;
    n->mtime = fs_now();
    generation++;
    n->changed = 1; n->wgen = generation;
    fs_unlock();
    return 0;
}

void fs_path(struct node *n, char *buf, size_t len)
{
    char tmp[1024];
    size_t pos = sizeof tmp - 1;
    tmp[pos] = 0;
    fs_lock();
    while (n && n != root && pos > 1) {
        size_t l = strlen(n->name);
        if (l + 1 > pos) break;
        pos -= l;
        memcpy(tmp + pos, n->name, l);
        tmp[--pos] = '/';
        n = n->parent;
    }
    fs_unlock();
    if (pos == sizeof tmp - 1) tmp[--pos] = '/';
    strlcpy(buf, tmp + pos, len);
}

/* ── tar (GNU and ustar) ────────────────────────────────────────────── */
static uint64_t octal(const char *s, int n)
{
    uint64_t v = 0;
    for (int i = 0; i < n && s[i]; i++) if (s[i] >= '0' && s[i] <= '7') v = v * 8 + (uint64_t)(s[i] - '0');
    return v;
}

/* make all directories of path (no trailing component) and return the parent */
static struct node *mkdirs(const char *path, size_t len)
{
    struct node *cur = root;
    size_t i = 0;
    while (i < len) {
        while (i < len && path[i] == '/') i++;
        size_t s = i;
        while (i < len && path[i] != '/') i++;
        if (i == s) break;
        if (i - s == 1 && path[s] == '.') continue;
        struct node *c = child_n(cur, path + s, i - s);
        if (!c) c = new_node(cur, path + s, i - s, N_DIR, 0755);
        else if (c->type == N_LNK) {
            struct node *t;
            char tg[256]; size_t tl = c->size < 255 ? (size_t)c->size : 255;
            memcpy(tg, c->data, tl); tg[tl] = 0;
            if (walk(cur, tg, 1, &t, 0) == 0) c = t;
        }
        if (!c) return 0;
        cur = c;
    }
    return cur;
}

int fs_mount_tar(const uint8_t *img, size_t len)
{
    fs_lock();
    mounting = 1;
    size_t off = 0;
    int count = 0;
    char longname[1024], longlink[1024];
    longname[0] = longlink[0] = 0;
    uint32_t now = fs_now();
    while (off + 512 <= len) {
        const char *h = (const char *)img + off;
        if (!h[0]) break;
        uint64_t size = octal(h + 124, 12);
        char type = h[156];
        const uint8_t *data = img + off + 512;
        size_t adv = 512 + (size_t)((size + 511) & ~511ULL);
        if (type == 'L' || type == 'K') {
            char *dst = type == 'L' ? longname : longlink;
            size_t n = size < 1023 ? (size_t)size : 1023;
            memcpy(dst, data, n); dst[n] = 0;
            off += adv;
            continue;
        }
        char name[1024];
        if (longname[0]) strlcpy(name, longname, sizeof name);
        else {
            char pre[156] = "", nm[101];
            memcpy(nm, h, 100); nm[100] = 0;
            if (!memcmp(h + 257, "ustar", 5)) { memcpy(pre, h + 345, 155); pre[155] = 0; }
            if (pre[0]) ksnprintf(name, sizeof name, "%s/%s", pre, nm); else strlcpy(name, nm, sizeof name);
        }
        char link[1024];
        if (longlink[0]) strlcpy(link, longlink, sizeof link);
        else { memcpy(link, h + 157, 100); link[100] = 0; }
        longname[0] = longlink[0] = 0;
        off += adv;

        const char *nm = name;
        if (nm[0] == '.' && nm[1] == '/') nm += 2;
        size_t nl = strlen(nm);
        while (nl && nm[nl - 1] == '/') nl--;
        if (!nl) continue;
        size_t s = nl;
        while (s && nm[s - 1] != '/') s--;
        struct node *dir = mkdirs(nm, s);
        if (!dir) continue;
        const char *base = nm + s;
        size_t bl = nl - s;
        uint16_t mode = (uint16_t)(octal(h + 100, 8) & 07777);
        uint32_t uid = (uint32_t)octal(h + 108, 8), gid = (uint32_t)octal(h + 116, 8);
        struct node *n = child_n(dir, base, bl);
        if (type == '5') {
            if (!n) n = new_node(dir, base, bl, N_DIR, mode);
            if (n) { n->mode = mode; n->mtime = now; }
        } else if (type == '2') {
            if (n) continue;
            n = new_node(dir, base, bl, N_LNK, 0777);
            if (n) { n->data = (uint8_t *)kstrdup(link, strlen(link)); n->size = strlen(link); n->cap = 0; n->mtime = now; }
            /* (symlink targets live in the heap, never in pages: cap stays 0) */
        } else if (type == '1') {
            struct node *t;
            if (walk(root, link, 0, &t, 0) == 0 && !n) {
                n = new_node(dir, base, bl, t->type, t->mode);
                if (n) { n->data = t->data; n->size = t->size; n->mtime = now; }
            }
        } else if (type == '0' || type == 0 || type == '7') {
            if (!n) n = new_node(dir, base, bl, N_REG, mode);
            if (n) { n->data = (uint8_t *)data; n->size = size; n->cap = 0; n->mode = mode; n->mtime = now; }
        }
        if (n && type != 'L' && type != 'K') { n->uid = uid; n->gid = gid; }
        count++;
    }
    mounting = 0;
    generation++;
    fs_unlock();
    return count;
}

void fs_init(void)
{
    root = new_node(0, "", 0, N_DIR, 0755);
    root->parent = root;
    static const char *const dirs[] = { "dev", "dev/shm", "dev/pts", "tmp", "home", "home/root", "proc", "run", "etc", "usr", "usr/bin", "var", "var/tmp",
                                        "sys", "sys/block", "sys/bus", "sys/class", "sys/dev", "sys/dev/char", "sys/dev/block", "sys/devices", 0 };
    for (int i = 0; dirs[i]; i++) mkdirs(dirs[i], strlen(dirs[i]));
    struct node *dev = fs_child(root, "dev");
    static const struct { const char *n; int d; } devs[] = {
        { "null", DEV_NULL }, { "zero", DEV_ZERO }, { "tty", DEV_TTY }, { "console", DEV_TTY }, { "tty0", DEV_TTY },
        { "random", DEV_RANDOM }, { "urandom", DEV_RANDOM }, { "fb0", DEV_FB }, { "full", DEV_FULL }, { 0, 0 } };
    for (int i = 0; devs[i].n; i++) {
        struct node *n = new_node(dev, devs[i].n, strlen(devs[i].n), N_CHR, 0666);
        if (n) n->dev = (uint32_t)devs[i].d;
    }
    struct node *t = fs_child(root, "tmp");
    if (t) t->mode = 01777;
}
