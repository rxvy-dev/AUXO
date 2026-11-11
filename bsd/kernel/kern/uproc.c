/* Processes and threads for user programs, files and pipes, and the terminal.
 * The system calls themselves are in sys.c. */
#include "kern.h"
#include "arch.h"
#include "rs.h"
#include "fs.h"
#include "uproc.h"
#include "unix.h"

#define MAXTASK 128
static struct task tasks[MAXTASK];
static volatile int fg_pgid;

struct task *task_by_pid(int pid)
{
    for (int i = 0; i < MAXTASK; i++) if (tasks[i].used && tasks[i].pid == pid) return &tasks[i];
    return 0;
}

struct task *task_current(void) { return task_by_pid(rs_current_pid()); }

int uproc_count(void)
{
    int n = 0;
    for (int i = 0; i < MAXTASK; i++) if (tasks[i].used && tasks[i].state != 2 && !tasks[i].is_thread) n++;
    return n;
}

struct task *task_alloc(void)
{
    uint64_t f;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(f) :: "memory");
    for (int i = 0; i < MAXTASK; i++)
        if (!tasks[i].used) {
            memset(&tasks[i], 0, sizeof tasks[i]);
            tasks[i].used = 1;
            tasks[i].pid = -1;
            if (f & 0x200) __asm__ volatile("sti");
            return &tasks[i];
        }
    if (f & 0x200) __asm__ volatile("sti");
    return 0;
}

void task_free(struct task *t) { t->used = 0; }

/* ── files ──────────────────────────────────────────────────────────── */
struct pipe { uint8_t *buf; uint32_t head, len, cap; int readers, writers; };

struct file *file_new(int kind, struct node *n, int flags)
{
    struct file *f = rs_kmalloc(sizeof *f);
    if (!f) return 0;
    memset(f, 0, sizeof *f);
    f->refs = 1;
    f->kind = kind;
    f->flags = flags;
    f->node = n;
    if (n) { fs_lock(); n->refs++; fs_unlock(); fs_path(n, f->path, sizeof f->path); }
    return f;
}

void file_put(struct file *f)
{
    if (!f || --f->refs > 0) return;
    if (f->kind == 1 || f->kind == 2) {                   /* pipe ends */
        struct pipe *p = f->priv;
        if (f->kind == 1) p->readers--; else p->writers--;
        if (!p->readers && !p->writers) { pmm_free_page((uint64_t)(uintptr_t)p->buf); rs_kfree(p); }
    }
    if (f->kind == 5 && f->priv) unix_close(f->priv);
    if ((f->kind == 6 || f->kind == 7) && f->priv) rs_kfree(f->priv);
    if (f->node) fs_put(f->node);
    rs_kfree(f);
}

int pipe_new(struct file **r, struct file **w)
{
    struct pipe *p = rs_kmalloc(sizeof *p);
    uint64_t pg = pmm_alloc_contig(16);                   /* 64 KiB, like Linux */
    if (!p || !pg) { if (p) rs_kfree(p); return -12; }
    memset(p, 0, sizeof *p);
    p->buf = (uint8_t *)(uintptr_t)pg;
    p->cap = 16 * PAGE_SIZE;
    p->readers = p->writers = 1;
    *r = file_new(1, 0, 0);
    *w = file_new(2, 0, 1);
    if (!*r || !*w) return -12;
    (*r)->priv = (*w)->priv = p;
    return 0;
}

int pipe_readable(struct file *f) { struct pipe *p = f->priv; return p->len > 0 || p->writers == 0; }
int pipe_writable(struct file *f) { struct pipe *p = f->priv; return p->len < p->cap || p->readers == 0; }

/* ── the terminal (stdin/stdout of programs started from auxsh) ─────── */
static char line[1024];
static int line_len, line_pos, line_ready, eof_pending;
static char pending_in[256];
static int npending;
int tty_icanon = 1, tty_echo = 1;

static int tty_getc_nb(void)
{
    if (npending) { int c = (unsigned char)pending_in[0]; memmove(pending_in, pending_in + 1, (size_t)--npending); return c; }
    return input_poll();
}

/* auxsh calls this while it waits for a program: Ctrl-C ends it, other keys are kept */
void tty_poll_intr(void)
{
    int c;
    while ((c = input_poll()) >= 0) {
        if (c == 3) { kprintf("^C\n"); uproc_kill_fg(2); continue; }
        if (npending < (int)sizeof pending_in) pending_in[npending++] = (char)c;
    }
}

int tty_readable(void)
{
    if (line_ready || eof_pending) return 1;
    if (!tty_icanon) return npending > 0 || input_peek() >= 0;
    return 0;
}

int tty_read(void *kbuf, size_t n, int nonblock)
{
    char *out = kbuf;
    if (!tty_icanon) {                                     /* raw: whatever is there, at least one */
        size_t got = 0;
        for (;;) {
            int c = tty_getc_nb();
            if (c >= 0) {
                if (c == 3 && tty_echo) { uproc_kill_fg(2); return -4; }
                if (c >= 0x80) {                             /* desktop key codes → VT100 */
                    const char *s = c == KEY_UP ? "\033[A" : c == KEY_DOWN ? "\033[B" : c == KEY_RIGHT ? "\033[C" : c == KEY_LEFT ? "\033[D" : "";
                    while (*s && got < n) out[got++] = *s++;
                } else out[got++] = (char)c;
                if (got >= n) return (int)got;
                continue;
            }
            if (got) return (int)got;
            if (nonblock) return -11;
            wait_tick();
        }
    }
    while (!line_ready && !eof_pending) {
        int c = tty_getc_nb();
        if (c < 0) { if (nonblock) return -11; wait_tick(); continue; }
        if (c == 3) { if (tty_echo) kprintf("^C\n"); line_len = 0; uproc_kill_fg(2); return -4; }
        if (c == 4) { if (line_len) { line_ready = 1; } else eof_pending = 1; continue; }
        if (c == '\b' || c == 0x7F) { if (line_len) { line_len--; if (tty_echo) kprintf("\b \b"); } continue; }
        if (c == 0x15) { while (line_len) { line_len--; if (tty_echo) kprintf("\b \b"); } continue; }
        if (c == '\r') c = '\n';
        if (c >= 0x80) continue;
        if (line_len < (int)sizeof line - 1) line[line_len++] = (char)c;
        if (tty_echo) { char ch = (char)c; console_write(&ch, 1); }
        if (c == '\n') line_ready = 1;
    }
    if (eof_pending && !line_ready) { eof_pending = 0; return 0; }
    size_t k = 0;
    while (k < n && line_pos < line_len) out[k++] = line[line_pos++];
    if (line_pos >= line_len) { line_len = line_pos = 0; line_ready = 0; }
    return (int)k;
}

long file_read(struct task *t, struct file *f, void *kbuf, size_t n)
{
    (void)t;
    if (f->kind == 5) {                                    /* socket */
        for (;;) {
            long r = unix_recv(f->priv, kbuf, n, 0, 0, 0);
            if (r != -11 || (f->flags & 04000)) return r;
            if (t && (t->pending & ~t->sigmask)) return -4;
            wait_tick();
        }
    }
    if (f->kind == 6) {                                    /* eventfd */
        uint64_t *cnt = f->priv;
        while (!*cnt) { if (f->flags & 04000) return -11; if (t && (t->pending & ~t->sigmask)) return -4; wait_tick(); }
        if (n < 8) return -22;
        memcpy(kbuf, cnt, 8);
        *cnt = 0;
        return 8;
    }
    if ((f->flags & 3) == 1) return -9;
    if (f->kind == 1) {
        struct pipe *p = f->priv;
        while (!p->len) {
            if (!p->writers) return 0;
            if (f->flags & 04000) return -11;
            wait_tick();
        }
        size_t k = n < p->len ? n : p->len;
        for (size_t i = 0; i < k; i++) ((uint8_t *)kbuf)[i] = p->buf[(p->head + i) % p->cap];
        p->head = (p->head + (uint32_t)k) % p->cap;
        p->len -= (uint32_t)k;
        return (long)k;
    }
    struct node *nd = f->node;
    if (!nd) return -9;
    if (nd->type == N_CHR) {
        switch (nd->dev) {
        case DEV_NULL: return 0;
        case DEV_ZERO: memset(kbuf, 0, n); return (long)n;
        case DEV_RANDOM:
            for (size_t i = 0; i < n; i++) ((uint8_t *)kbuf)[i] = (uint8_t)((rdtsc() * 6364136223846793005ULL + i * 1442695040888963407ULL) >> 33);
            return (long)n;
        case DEV_TTY: return tty_read(kbuf, n, f->flags & 04000);
        default: return -19;
        }
    }
    if (nd->type == N_DIR) return -21;
    long r = fs_read(nd, f->pos, kbuf, n);
    if (r > 0) f->pos += (uint64_t)r;
    return r;
}

long file_write(struct task *t, struct file *f, const void *kbuf, size_t n)
{
    (void)t;
    if (f->kind == 5) {
        size_t done = 0;
        while (done < n) {
            long r = unix_send(f->priv, (const uint8_t *)kbuf + done, n - done, 0, 0);
            if (r == -11) { if (f->flags & 04000) return done ? (long)done : -11; wait_tick(); continue; }
            if (r < 0) return done ? (long)done : r;
            done += (size_t)r;
        }
        return (long)done;
    }
    if (f->kind == 6) {
        if (n < 8) return -22;
        uint64_t v; memcpy(&v, kbuf, 8);
        *(uint64_t *)f->priv += v;
        return 8;
    }
    if ((f->flags & 3) == 0 && f->kind != 2) return -9;
    if (f->kind == 2) {
        struct pipe *p = f->priv;
        size_t done = 0;
        while (done < n) {
            if (!p->readers) return done ? (long)done : -32;   /* EPIPE */
            if (p->len == p->cap) { if (f->flags & 04000) return done ? (long)done : -11; wait_tick(); continue; }
            size_t room = p->cap - p->len, k = n - done < room ? n - done : room;
            for (size_t i = 0; i < k; i++) p->buf[(p->head + p->len + i) % p->cap] = ((const uint8_t *)kbuf)[done + i];
            p->len += (uint32_t)k;
            done += k;
        }
        return (long)done;
    }
    struct node *nd = f->node;
    if (!nd) return -9;
    if (nd->type == N_CHR) {
        if (nd->dev == DEV_TTY) { console_write(kbuf, n); return (long)n; }
        return (long)n;                                        /* null, zero, random swallow */
    }
    if (f->flags & 02000) f->pos = nd->size;                    /* O_APPEND */
    long r = fs_write(nd, f->pos, kbuf, n);
    if (r > 0) f->pos += (uint64_t)r;
    return r;
}

/* ── starting programs from the kernel shell ────────────────────────── */
struct start { struct task *t; char **argv, **envp; char path[256]; int ok; };

static char **copy_vec(char *const v[])
{
    int n = 0;
    size_t bytes = 0;
    while (v && v[n]) { bytes += strlen(v[n]) + 1; n++; }
    char **out = rs_kmalloc(sizeof(char *) * (size_t)(n + 1) + bytes);
    if (!out) return 0;
    char *s = (char *)(out + n + 1);
    for (int i = 0; i < n; i++) { size_t l = strlen(v[i]) + 1; memcpy(s, v[i], l); out[i] = s; s += l; }
    out[n] = 0;
    return out;
}

void task_exit(struct task *t, int status) __attribute__((noreturn));

static void user_start(uintptr_t arg)
{
    struct start *st = (struct start *)arg;
    struct task *t = st->t;
    t->pid = t->tgid = t->pgid = rs_current_pid();
    t->start_tick = kernel_ticks();
    if (!t->detached) fg_pgid = t->pgid;
    struct trapframe tf;
    int r = exec_load(t, st->path, st->argv, st->envp, &tf);
    rs_kfree(st->argv);
    rs_kfree(st->envp);
    if (r < 0) {
        st->ok = r;
        rs_kfree(st);
        task_exit(t, 127 << 8);
    }
    st->ok = 1;
    rs_kfree(st);
    user_return(&tf);
}

static int spawn(const char *path, char *const argv[], char *const envp[], const char *cwd, int detached);
int uproc_spawn(const char *path, char *const argv[], char *const envp[], const char *cwd) { return spawn(path, argv, envp, cwd, 0); }
int uproc_spawn_detached(const char *path, char *const argv[], char *const envp[], const char *cwd) { return spawn(path, argv, envp, cwd, 1); }

static int spawn(const char *path, char *const argv[], char *const envp[], const char *cwd, int detached)
{
    struct task *t = task_alloc();
    if (!t) return -11;
    t->ppid = 0;
    t->detached = detached;
    t->umask = 022;
    t->files = rs_kmalloc(sizeof *t->files);
    t->sig = rs_kmalloc(sizeof(struct sigact) * 64);
    if (!t->files || !t->sig) { task_free(t); return -12; }
    memset(t->files, 0, sizeof *t->files);
    memset(t->sig, 0, sizeof(struct sigact) * 64);
    t->files->refs = 1;
    struct node *tty, *wd;
    if (fs_lookup(0, "/dev/tty", 1, &tty) < 0) { task_free(t); return -2; }
    struct file *f = file_new(0, tty, 2);
    if (!f) { task_free(t); return -12; }
    t->files->fd[0] = f; t->files->fd[1] = f; t->files->fd[2] = f;
    f->refs = 3;
    if (fs_lookup(0, cwd && cwd[0] ? cwd : "/home/root", 1, &wd) < 0) fs_lookup(0, "/", 1, &wd);
    t->cwd = wd;
    struct start *st = rs_kmalloc(sizeof *st);
    if (!st) { task_free(t); return -12; }
    st->t = t;
    st->argv = copy_vec(argv);
    st->envp = copy_vec(envp);
    st->ok = 0;
    strlcpy(st->path, path, sizeof st->path);
    const char *b = strrchr(path, '/');
    char nm[16];
    strlcpy(nm, b ? b + 1 : path, sizeof nm);
    int pid = rs_spawn(nm, user_start, (uintptr_t)st);
    if (pid < 0) { task_free(t); return -11; }
    for (int i = 0; i < 1000 && t->pid != pid && !detached; i++) rs_yield();
    return pid;
}

int uproc_wait(int pid, int *status)
{
    for (;;) {
        struct task *t = task_by_pid(pid);
        if (!t) return -10;
        if (t->state == 2) {
            *status = t->exit_status;
            task_free(t);
            fg_pgid = 0;
            return pid;
        }
        tty_poll_intr();
        wait_tick();
    }
}

void uproc_kill_fg(int sig)
{
    if (!fg_pgid) return;
    for (int i = 0; i < MAXTASK; i++)
        if (tasks[i].used && tasks[i].state != 2 && tasks[i].pgid == fg_pgid) tasks[i].pending |= 1ULL << (sig - 1);
}

void uproc_set_fg(int pgid) { fg_pgid = pgid; }

/* iterate tasks for sys.c */
struct task *task_slot(int i) { return i >= 0 && i < MAXTASK ? &tasks[i] : 0; }
