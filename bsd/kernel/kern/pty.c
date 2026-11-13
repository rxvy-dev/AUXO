/* Pseudo-terminals: /dev/ptmx and /dev/pts/N, as on Linux. A terminal emulator (foot,
 * GNOME Console, xterm), ssh, tmux, screen, script, expect or an IDE opens /dev/ptmx and gets
 * the master side; the shell it runs gets the slave, which behaves like a real terminal:
 *   - the line discipline: line editing in canonical mode (erase, kill, word erase, EOF),
 *     echo, CR/NL translation in and out, raw mode with VMIN/VTIME for editors;
 *   - ^C, ^\ and ^Z send SIGINT, SIGQUIT and SIGTSTP to the foreground process group;
 *   - window sizes (TIOCSWINSZ sends SIGWINCH), the controlling terminal of a session
 *     (setsid, TIOCSCTTY, /dev/tty), foreground groups (tcsetpgrp);
 *   - closing the master hangs up the slave (SIGHUP, then EOF / EIO). */
#include "kern.h"
#include "arch.h"
#include "rs.h"
#include "fs.h"
#include "uproc.h"
#include "cred.h"
#include "pty.h"

#define MAXPTY 64
#define QCAP 8192

/* termios bits */
#define IGNCR 0x80
#define ICRNL 0x100
#define INLCR 0x40
#define ISTRIP 0x20
#define IUTF8 0x4000
#define OPOST 0x1
#define ONLCR 0x4
#define OCRNL 0x8
#define ISIG 0x1
#define ICANON 0x2
#define ECHO 0x8
#define ECHOE 0x10
#define ECHOK 0x20
#define ECHONL 0x40
#define NOFLSH 0x80
#define ECHOCTL 0x200
#define IEXTEN 0x8000
enum { VINTR, VQUIT, VERASE, VKILL, VEOF, VTIME, VMIN, VSWTC, VSTART, VSTOP, VSUSP, VEOL, VREPRINT, VDISCARD, VWERASE, VLNEXT, VEOL2 };

struct q { uint8_t b[QCAP]; uint32_t head, len; };
struct pty {
    int used, locked, master, slaves, opened_slave, hung;
    uint32_t iflag, oflag, cflag, lflag;
    uint8_t line_disc, cc[19];
    uint16_t rows, cols, xpix, ypix;
    struct q in, out;                  /* in: for the slave to read; out: for the master to read */
    uint8_t line[4096]; uint32_t llen; /* the line being edited (canonical mode) */
    int eof;                           /* an EOF (^D on an empty line) for the slave's next read */
    int lnext;                         /* the next byte is taken literally (^V) */
    int fg, sid;                       /* foreground process group, session (global ids) */
    struct node *node;                 /* /dev/pts/N */
};
static struct pty ptys[MAXPTY];

static uint32_t q_room(const struct q *q) { return QCAP - q->len; }
static void q_put(struct q *q, uint8_t c) { if (q->len < QCAP) { q->b[(q->head + q->len) % QCAP] = c; q->len++; } }
static int q_get(struct q *q) { if (!q->len) return -1; int c = q->b[q->head]; q->head = (q->head + 1) % QCAP; q->len--; return c; }

static void defaults(struct pty *p)
{
    p->iflag = ICRNL | IUTF8 | 0x400;           /* (IXON) */
    p->oflag = OPOST | ONLCR;
    p->cflag = 0xBF | 0x800;                     /* B38400, CS8, CREAD, HUPCL */
    p->lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHOCTL | IEXTEN | 0x800;   /* (ECHOKE) */
    static const uint8_t cc[19] = { 3, 28, 127, 21, 4, 0, 1, 0, 17, 19, 26, 0, 18, 15, 23, 22, 0, 0, 0 };
    memcpy(p->cc, cc, sizeof cc);
    p->rows = 24; p->cols = 80;
}

/* send a signal to a process group (global id) */
static void signal_group(int pgid, int sig)
{
    if (pgid <= 0) return;
    for (int i = 0; i < MAXTASK; i++) {
        struct task *t = task_slot(i);
        if (t->used && t->state != 2 && t->pgid == pgid) t->pending |= 1ULL << (sig - 1);
    }
}

/* output processing, into the master's queue */
static void out_byte(struct pty *p, uint8_t c)
{
    if (p->oflag & OPOST) {
        if (c == '\n' && (p->oflag & ONLCR)) { q_put(&p->out, '\r'); q_put(&p->out, '\n'); return; }
        if (c == '\r' && (p->oflag & OCRNL)) { q_put(&p->out, '\n'); return; }
    }
    q_put(&p->out, c);
}

static void echo_byte(struct pty *p, uint8_t c)
{
    if (!(p->lflag & ECHO)) { if (c == '\n' && (p->lflag & ECHONL)) out_byte(p, c); return; }
    if ((p->lflag & ECHOCTL) && c < 0x20 && c != '\n' && c != '\t' && c != '\r') { out_byte(p, '^'); out_byte(p, (uint8_t)(c + 64)); return; }
    if ((p->lflag & ECHOCTL) && c == 0x7F) { out_byte(p, '^'); out_byte(p, '?'); return; }
    out_byte(p, c);
}

static void erase_one(struct pty *p)
{
    if (!p->llen) return;
    uint8_t c = p->line[--p->llen];
    /* a UTF-8 character is one erase */
    if (p->iflag & IUTF8) while (p->llen && (p->line[p->llen] & 0xC0) == 0x80 && (c & 0xC0) == 0x80) c = p->line[--p->llen];
    if ((p->lflag & ECHO) && (p->lflag & ECHOE)) {
        int w = (p->lflag & ECHOCTL) && (c < 0x20 || c == 0x7F) && c != '\t' ? 2 : 1;
        for (int i = 0; i < w; i++) { out_byte(p, '\b'); out_byte(p, ' '); out_byte(p, '\b'); }
    }
}

static void push_line(struct pty *p)
{
    for (uint32_t i = 0; i < p->llen; i++) q_put(&p->in, p->line[i]);
    p->llen = 0;
}

/* one byte typed into the terminal (written to the master) */
static void input_byte(struct pty *p, uint8_t c)
{
    if (p->lnext) { p->lnext = 0; goto literal; }
    if (p->iflag & ISTRIP) c &= 0x7F;
    if (c == '\r') { if (p->iflag & IGNCR) return; if (p->iflag & ICRNL) c = '\n'; }
    else if (c == '\n' && (p->iflag & INLCR)) c = '\r';
    if (p->lflag & ISIG) {
        int sig = c == p->cc[VINTR] ? 2 : c == p->cc[VQUIT] ? 3 : c == p->cc[VSUSP] ? 20 : 0;
        if (sig && c) {
            if (!(p->lflag & NOFLSH)) { p->in.len = 0; p->llen = 0; }
            echo_byte(p, c);
            if (p->lflag & ECHO) out_byte(p, '\n');
            signal_group(p->fg, sig);
            return;
        }
    }
    if ((p->lflag & IEXTEN) && c == p->cc[VLNEXT] && c) { p->lnext = 1; return; }
    if (p->lflag & ICANON) {
        if (c == p->cc[VERASE] || c == '\b') { erase_one(p); return; }
        if (c == p->cc[VKILL] && c) { while (p->llen) erase_one(p); return; }
        if ((p->lflag & IEXTEN) && c == p->cc[VWERASE] && c) {
            while (p->llen && (p->line[p->llen - 1] == ' ' || p->line[p->llen - 1] == '\t')) erase_one(p);
            while (p->llen && p->line[p->llen - 1] != ' ' && p->line[p->llen - 1] != '\t') erase_one(p);
            return;
        }
        if (c == p->cc[VEOF] && c) { if (p->llen) push_line(p); else p->eof++; return; }
        if ((p->lflag & IEXTEN) && c == p->cc[VREPRINT] && c) {
            out_byte(p, '^'); out_byte(p, 'R'); out_byte(p, '\n');
            for (uint32_t i = 0; i < p->llen; i++) echo_byte(p, p->line[i]);
            return;
        }
literal:
        if (p->lflag & ICANON) {
            if (p->llen < sizeof p->line - 1) p->line[p->llen++] = c;
            echo_byte(p, c);
            if (c == '\n' || (c == p->cc[VEOL] && c) || (c == p->cc[VEOL2] && c)) push_line(p);
            return;
        }
    }
    q_put(&p->in, c);
    echo_byte(p, c);
}

static struct pty *of(struct file *f)
{
    uintptr_t i = 0;
    if (f->node && f->node->type == N_CHR && f->node->dev == DEV_PTMX) i = (uintptr_t)f->priv;
    else if (f->node && f->node->type == N_CHR && f->node->dev == DEV_PTS) i = (uintptr_t)f->node->priv;
    return i >= 1 && i <= MAXPTY && ptys[i - 1].used ? &ptys[i - 1] : 0;
}
static int is_master(struct file *f) { return f->node && f->node->dev == DEV_PTMX; }

int pty_is(struct file *f) { return f->node && f->node->type == N_CHR && (f->node->dev == DEV_PTMX || f->node->dev == DEV_PTS); }

/* open /dev/ptmx: a new pair; or /dev/pts/N: its slave */
int pty_open(struct task *t, struct file *f, int flags)
{
    if (f->node->dev == DEV_PTMX) {
        int i = 0;
        while (i < MAXPTY && ptys[i].used) i++;
        if (i == MAXPTY) return -28;                            /* ENOSPC */
        struct pty *p = &ptys[i];
        memset(p, 0, sizeof *p);
        p->used = 1; p->locked = 1; p->master = 1;
        defaults(p);
        struct node *dev, *pts;
        if (fs_lookup(0, "/dev/pts", 1, &pts) == 0 || (fs_lookup(0, "/dev", 1, &dev) == 0 && (pts = fs_create(dev, "pts", N_DIR, 0755)))) {
            char nm[8]; ksnprintf(nm, sizeof nm, "%d", i);
            struct node *n = fs_child(pts, nm);
            if (!n) n = fs_create(pts, nm, N_CHR, 0620);
            if (n) {
                n->dev = DEV_PTS; n->priv = (void *)(uintptr_t)(i + 1); n->changed = 0;
                n->uid = t ? t->ns.cred.fsuid : 0; n->gid = 5;   /* the owner's, group tty */
                n->mode = 0620;
                p->node = n;
            }
        }
        f->priv = (void *)(uintptr_t)(i + 1);
        return 0;
    }
    struct pty *p = of(f);
    if (!p || !p->master) return -5;                          /* EIO: no such terminal (or hung up) */
    if (p->locked) return -5;
    p->slaves++; p->opened_slave = 1;
    /* a session leader without a terminal takes this one (unless O_NOCTTY) */
    if (t && !(flags & 0400) && t->sid == t->tgid && !t->ctty && !p->sid) {
        t->ctty = (int)(p - ptys) + 1; p->sid = t->sid; p->fg = t->pgid;
    }
    return 0;
}

void pty_close(struct file *f)
{
    struct pty *p = of(f);
    if (!p) return;
    if (is_master(f)) {
        p->master = 0;
        if (p->sid) { signal_group(p->fg, 1); signal_group(p->sid, 1); }   /* SIGHUP: the terminal went */
        p->hung = 1;
        for (int i = 0; i < MAXTASK; i++) { struct task *x = task_slot(i); if (x->used && x->ctty == (int)(p - ptys) + 1) x->ctty = 0; }
        if (!p->slaves) {
            if (p->node) { struct node *pts; if (fs_lookup(0, "/dev/pts", 1, &pts) == 0) fs_unlink(pts, p->node->name, 0); }
            p->used = 0;
        }
    } else {
        if (p->slaves) p->slaves--;
        if (!p->slaves && !p->master) {
            if (p->node) { struct node *pts; if (fs_lookup(0, "/dev/pts", 1, &pts) == 0) fs_unlink(pts, p->node->name, 0); }
            p->used = 0;
        }
    }
}

long pty_read(struct task *t, struct file *f, void *buf, size_t n)
{
    struct pty *p = of(f);
    if (!p) return -5;
    uint8_t *b = buf;
    if (is_master(f)) {
        for (;;) {
            if (p->out.len) {
                size_t k = 0;
                while (k < n && p->out.len) b[k++] = (uint8_t)q_get(&p->out);
                return (long)k;
            }
            if (p->opened_slave && !p->slaves) return -5;     /* EIO: everything on the other side closed */
            if (f->flags & 04000) return -11;
            if (t->pending & ~t->sigmask) return -4;
            wait_tick();
        }
    }
    uint64_t until = 0;
    int vmin = p->cc[VMIN], vtime = p->cc[VTIME];
    if (!(p->lflag & ICANON) && vtime) until = kernel_ticks() + (uint64_t)vtime * AUXO_HZ / 10;
    for (;;) {
        if (p->lflag & ICANON) {
            if (p->in.len) {
                size_t k = 0;                                  /* up to the end of one line */
                while (k < n && p->in.len) { int c = q_get(&p->in); b[k++] = (uint8_t)c; if (c == '\n') break; }
                return (long)k;
            }
            if (p->eof) { p->eof--; return 0; }
        } else {
            if (p->in.len && (int)p->in.len >= (vmin ? vmin : 1)) {
                size_t k = 0;
                while (k < n && p->in.len) b[k++] = (uint8_t)q_get(&p->in);
                return (long)k;
            }
            if (!vmin && !vtime) { size_t k = 0; while (k < n && p->in.len) b[k++] = (uint8_t)q_get(&p->in); return (long)k; }
            if (until && kernel_ticks() >= until) { size_t k = 0; while (k < n && p->in.len) b[k++] = (uint8_t)q_get(&p->in); return (long)k; }
        }
        if (p->hung || !p->master) return 0;                  /* hung up: end of file */
        if (f->flags & 04000) return -11;
        if (t->pending & ~t->sigmask) return -4;
        wait_tick();
    }
}

long pty_write(struct task *t, struct file *f, const void *buf, size_t n)
{
    struct pty *p = of(f);
    if (!p) return -5;
    const uint8_t *b = buf;
    size_t done = 0;
    if (is_master(f)) {
        while (done < n) {
            if (q_room(&p->in) < 8 || q_room(&p->out) < 16) {  /* (input echoes into out) */
                if (f->flags & 04000) return done ? (long)done : -11;
                if (t->pending & ~t->sigmask) return done ? (long)done : -4;
                wait_tick();
                continue;
            }
            input_byte(p, b[done++]);
        }
        return (long)done;
    }
    if (p->hung || !p->master) return -5;                     /* EIO */
    while (done < n) {
        if (q_room(&p->out) < 2) {
            if (!p->master) return done ? (long)done : -5;
            if (f->flags & 04000) return done ? (long)done : -11;
            if (t->pending & ~t->sigmask) return done ? (long)done : -4;
            wait_tick();
            continue;
        }
        out_byte(p, b[done++]);
    }
    return (long)done;
}

/* poll: 1 readable, 4 writable, 8|16 error / hang-up */
int pty_ready(struct file *f)
{
    struct pty *p = of(f);
    if (!p) return 8;
    int r = 0;
    if (is_master(f)) {
        if (p->out.len) r |= 1;
        if (q_room(&p->in) >= 8) r |= 4;
        if (p->opened_slave && !p->slaves) r |= 16 | 1;
        return r;
    }
    if ((p->lflag & ICANON) ? (p->in.len || p->eof) : p->in.len) r |= 1;
    if (q_room(&p->out) >= 2) r |= 4;
    if (p->hung || !p->master) r |= 16 | 1;
    return r;
}

long pty_ioctl(struct task *t, struct file *f, uint64_t req, uint64_t arg)
{
    struct pty *p = of(f);
    if (!p) return -5;
    switch (req) {
    case 0x80045430: { int n = (int)(p - ptys); return copyout(t->mm, arg, &n, 4) < 0 ? -14 : 0; }   /* TIOCGPTN */
    case 0x40045431: { int v; if (copyin(t->mm, &v, arg, 4) < 0) return -14; p->locked = v != 0; return 0; }   /* TIOCSPTLCK */
    case 0x80045439: { int v = p->locked; return copyout(t->mm, arg, &v, 4) < 0 ? -14 : 0; }   /* TIOCGPTLCK */
    case 0x5401: {                                             /* TCGETS */
        uint8_t b[36];
        memcpy(b, &p->iflag, 4); memcpy(b + 4, &p->oflag, 4); memcpy(b + 8, &p->cflag, 4); memcpy(b + 12, &p->lflag, 4);
        b[16] = p->line_disc; memcpy(b + 17, p->cc, 19);
        return copyout(t->mm, arg, b, sizeof b) < 0 ? -14 : 0;
    }
    case 0x5402: case 0x5403: case 0x5404: {                   /* TCSETS, TCSETSW, TCSETSF */
        uint8_t b[36];
        if (copyin(t->mm, b, arg, sizeof b) < 0) return -14;
        uint32_t ol = p->lflag;
        memcpy(&p->iflag, b, 4); memcpy(&p->oflag, b + 4, 4); memcpy(&p->cflag, b + 8, 4); memcpy(&p->lflag, b + 12, 4);
        p->line_disc = b[16]; memcpy(p->cc, b + 17, 19);
        if (req == 0x5404) { p->in.len = 0; p->llen = 0; }
        if ((ol & ICANON) && !(p->lflag & ICANON)) push_line(p);   /* to raw: what was typed is readable */
        return 0;
    }
    case 0x5413: {                                             /* TIOCGWINSZ */
        uint16_t w[4] = { p->rows, p->cols, p->xpix, p->ypix };
        return copyout(t->mm, arg, w, 8) < 0 ? -14 : 0;
    }
    case 0x5414: {                                             /* TIOCSWINSZ */
        uint16_t w[4];
        if (copyin(t->mm, w, arg, 8) < 0) return -14;
        int changed = w[0] != p->rows || w[1] != p->cols;
        p->rows = w[0]; p->cols = w[1]; p->xpix = w[2]; p->ypix = w[3];
        if (changed) signal_group(p->fg, 28);                  /* SIGWINCH */
        return 0;
    }
    case 0x540F: {                                             /* TIOCGPGRP */
        int g = 0;
        if (p->fg) { struct task *x = task_by_pid(p->fg); g = x ? pid_vtgid(t, x) : 0; }
        return copyout(t->mm, arg, &g, 4) < 0 ? -14 : 0;
    }
    case 0x5410: {                                             /* TIOCSPGRP */
        int g; if (copyin(t->mm, &g, arg, 4) < 0) return -14;
        int gg = pid_global(t, g, 1);
        if (!gg) return -3;
        p->fg = gg;
        return 0;
    }
    case 0x5429: { int s = p->sid; struct task *x = s ? task_by_pid(s) : 0; s = x ? pid_vtgid(t, x) : 0; return copyout(t->mm, arg, &s, 4) < 0 ? -14 : 0; }   /* TIOCGSID */
    case 0x540E:                                               /* TIOCSCTTY */
        if (t->sid != t->tgid) return -1;
        if (p->sid && p->sid != t->sid && !(arg == 1 && ns_capable(t, 21))) return -1;
        t->ctty = (int)(p - ptys) + 1; p->sid = t->sid; p->fg = t->pgid;
        return 0;
    case 0x5422:                                               /* TIOCNOTTY */
        if (t->ctty == (int)(p - ptys) + 1) { if (t->sid == t->tgid) { signal_group(p->fg, 1); p->sid = 0; p->fg = 0; } t->ctty = 0; }
        return 0;
    case 0x541B: { int n = (int)(is_master(f) ? p->out.len : p->in.len); return copyout(t->mm, arg, &n, 4) < 0 ? -14 : 0; }   /* FIONREAD */
    case 0x540B: {                                             /* TCFLSH */
        if (arg == 0 || arg == 2) { p->in.len = 0; p->llen = 0; }
        if (arg == 1 || arg == 2) p->out.len = 0;
        return 0;
    }
    case 0x5409: case 0x540A: case 0x5425: case 0x5427: case 0x5428: return 0;   /* TCSBRK, TCXONC, TCGETX… */
    case 0x5441: return -22;                                   /* TIOCGPTPEER: handled by the caller */
    case 0x5421: return 0;                                     /* FIONBIO: (the caller sets the flag) */
    case 0x5437: case 0x5438: return -22;                      /* TIOCVHANGUP and friends */
    }
    return -25;                                                /* ENOTTY */
}

/* /dev/tty for a process whose controlling terminal is a pty: its slave */
struct node *pty_ctty_node(struct task *t)
{
    if (!t || !t->ctty || t->ctty > MAXPTY || !ptys[t->ctty - 1].used) return 0;
    return ptys[t->ctty - 1].node;
}

void pty_setsid(struct task *t) { t->ctty = 0; }

/* the slave node of a master (TIOCGPTPEER) */
struct node *pty_peer(struct file *f) { struct pty *p = of(f); return p && is_master(f) ? p->node : 0; }

uint64_t pty_rdev(struct node *n) { return n->dev == DEV_PTMX ? (5u << 8) | 2 : (136u << 8) | (uint64_t)((uintptr_t)n->priv - 1); }
