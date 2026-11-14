/* 32-bit (i386) Linux programs: system calls, TLS and signals.
 *
 * A 32-bit program runs in compatibility mode (CS 0x3b) and asks for system calls with
 * `int $0x80`: number in eax, arguments in ebx ecx edx esi edi ebp. Most calls are the x86_64
 * ones under another number, with their arguments widened (sign-extended where they can be
 * negative: file descriptors, pids, AT_FDCWD). Where a structure differs between the two ABIs
 * (stat64, iovec, msghdr, timespec with a 32-bit time_t, sigaction …) we convert it, using a
 * scratch area on the program's own stack below its stack pointer (i386 has no red zone, so
 * nothing lives there), then make the x86_64 call on the converted copy.
 *
 * Thread-local storage: glibc's i386 ABI points %gs at a GDT descriptor set with
 * set_thread_area. Each thread has three (GDT entries 8-10, selectors 0x43/0x4b/0x53); the
 * scheduler swaps them in with the thread (rs_set_tls / arch_tls_switch). */
#include "kern.h"
#include "rs.h"
#include "uproc.h"

long syscall_call(struct task *t, struct trapframe *tf, long nr, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f);
int  seccomp_check_arch(struct task *t, long nr, const uint64_t args[6], uint64_t ip, long *ret, int compat);
void uproc_signals(struct task *t, struct trapframe *tf);
void compat_exit(struct task *t, int sig) __attribute__((noreturn));

enum { C_EPERM = 1, C_EFAULT = 14, C_EINVAL = 22, C_ENOSYS = 38, C_ESRCH = 3, C_ENOMEM = 12 };

#define S64(nr, a, b, c, d, e, f) syscall_call(t, tf, (nr), (uint64_t)(a), (uint64_t)(b), (uint64_t)(c), (uint64_t)(d), (uint64_t)(e), (uint64_t)(f))
#define SX(v) ((uint64_t)(int64_t)(int32_t)(uint32_t)(v))       /* a 32-bit int, sign-extended */

/* ── scratch space on the user's stack ──────────────────────────────── */
struct scratch { uint64_t at; };
static uint64_t scr_alloc(struct scratch *s, size_t n)
{
    s->at = (s->at - n) & ~15ULL;
    return s->at;
}
static void scr_init(struct scratch *s, struct trapframe *tf) { s->at = ((tf->rsp & 0xFFFFFFFFULL) - 256) & ~15ULL; }

static int in32(struct task *t, void *k, uint64_t u, size_t n) { return copyin(t->mm, k, u, n); }
static int out32(struct task *t, uint64_t u, const void *k, size_t n) { return copyout(t->mm, u, k, n); }

/* ── TLS: set_thread_area / get_thread_area ─────────────────────────── */
struct user_desc { uint32_t entry, base, limit, flags; };
#define TLS_MIN 8
#define TLS_N   3

static uint64_t desc_from(const struct user_desc *u)
{
    uint32_t fl = u->flags;
    int seg32 = fl & 1, contents = (fl >> 1) & 3, rx_only = (fl >> 3) & 1, pages = (fl >> 4) & 1,
        not_present = (fl >> 5) & 1, useable = (fl >> 6) & 1;
    if (!u->base && !u->limit && rx_only && not_present && !seg32 && !contents && !pages && !useable) return 0;   /* empty */
    uint64_t type = ((uint64_t)(rx_only ^ 1) << 1) | ((uint64_t)contents << 2) | 1;
    uint64_t d = (u->limit & 0xFFFFULL) | ((uint64_t)(u->base & 0xFFFFFF) << 16) | (type << 40) | (1ULL << 44) | (3ULL << 45) |
                 ((uint64_t)(not_present ^ 1) << 47) | ((uint64_t)((u->limit >> 16) & 0xF) << 48) | ((uint64_t)useable << 52) |
                 ((uint64_t)seg32 << 54) | ((uint64_t)pages << 55) | ((uint64_t)(u->base >> 24) << 56);
    return d;
}

static void desc_to(uint64_t d, struct user_desc *u)
{
    u->base = (uint32_t)(((d >> 16) & 0xFFFFFF) | ((d >> 56) << 24));
    u->limit = (uint32_t)((d & 0xFFFF) | (((d >> 48) & 0xF) << 16));
    uint32_t type = (uint32_t)(d >> 40) & 0xF;
    u->flags = (uint32_t)((d >> 54) & 1) | (((type >> 2) & 3) << 1) | ((((type >> 1) & 1) ^ 1) << 3) | ((uint32_t)((d >> 55) & 1) << 4) |
               ((uint32_t)(((d >> 47) & 1) ^ 1) << 5) | ((uint32_t)((d >> 52) & 1) << 6);
    if (!d) u->flags = (1 << 3) | (1 << 5);
}

/* fill tls[] from the user_desc at uinfo; picks a free entry for entry_number -1 and writes the
 * choice back. Used by set_thread_area and clone(CLONE_SETTLS). */
long compat_tls_desc(struct task *t, uint64_t uinfo, uint64_t tls[3])
{
    struct user_desc u;
    if (in32(t, &u, uinfo, sizeof u) < 0) return -C_EFAULT;
    if (((u.flags >> 1) & 3) == 3) return -C_EINVAL;          /* conforming code: no */
    if (u.entry == 0xFFFFFFFFu) {
        for (int i = 0; i < TLS_N; i++) if (!tls[i]) { u.entry = TLS_MIN + (uint32_t)i; break; }
        if (u.entry == 0xFFFFFFFFu) return -C_ESRCH;
        if (out32(t, uinfo, &u.entry, 4) < 0) return -C_EFAULT;
    }
    if (u.entry < TLS_MIN || u.entry >= TLS_MIN + TLS_N) return -C_EINVAL;
    tls[u.entry - TLS_MIN] = desc_from(&u);
    return 0;
}

static long set_thread_area(struct task *t, uint64_t uinfo)
{
    uint64_t tls[3];
    rs_get_tls(tls);
    long r = compat_tls_desc(t, uinfo, tls);
    if (r < 0) return r;
    rs_set_tls(tls, -1);                                      /* reloads %gs if it points there */
    return 0;
}

static long get_thread_area(struct task *t, uint64_t uinfo)
{
    struct user_desc u;
    if (in32(t, &u, uinfo, 4) < 0) return -C_EFAULT;
    if (u.entry < TLS_MIN || u.entry >= TLS_MIN + TLS_N) return -C_EINVAL;
    uint64_t tls[3];
    rs_get_tls(tls);
    uint32_t e = u.entry;
    desc_to(tls[e - TLS_MIN], &u);
    u.entry = e;
    return out32(t, uinfo, &u, sizeof u) < 0 ? -C_EFAULT : 0;
}

/* ── structure conversion ───────────────────────────────────────────── */
struct ts32 { int32_t s, ns; };
struct ts64 { int64_t s, ns; };

/* a 32-bit timespec/timeval at u (0 stays 0) → a 64-bit one in scratch */
static long ts_in(struct task *t, struct scratch *s, uint64_t u, int count, uint64_t *out)
{
    if (!u) { *out = 0; return 0; }
    struct ts32 a[4];
    struct ts64 b[4];
    if (in32(t, a, u, sizeof(struct ts32) * (size_t)count) < 0) return -C_EFAULT;
    for (int i = 0; i < count; i++) { b[i].s = a[i].s; b[i].ns = a[i].ns; }
    *out = scr_alloc(s, sizeof(struct ts64) * (size_t)count);
    return out32(t, *out, b, sizeof(struct ts64) * (size_t)count) < 0 ? -C_EFAULT : 0;
}

static long ts_out(struct task *t, uint64_t k64, uint64_t u, int count)
{
    if (!u || !k64) return 0;
    struct ts64 b[4];
    struct ts32 a[4];
    if (in32(t, b, k64, sizeof(struct ts64) * (size_t)count) < 0) return -C_EFAULT;
    for (int i = 0; i < count; i++) { a[i].s = (int32_t)b[i].s; a[i].ns = (int32_t)b[i].ns; }
    return out32(t, u, a, sizeof(struct ts32) * (size_t)count) < 0 ? -C_EFAULT : 0;
}

/* iovec arrays: 8 bytes each in i386, 16 in x86_64 */
static long iov_in(struct task *t, struct scratch *s, uint64_t u, uint64_t n, uint64_t *out)
{
    if (n > 1024) return -C_EINVAL;
    *out = scr_alloc(s, 16 * (size_t)n + 16);
    for (uint64_t i = 0; i < n; i++) {
        uint32_t v[2];
        if (in32(t, v, u + i * 8, 8) < 0) return -C_EFAULT;
        uint64_t w[2] = { v[0], v[1] };
        if (out32(t, *out + i * 16, w, 16) < 0) return -C_EFAULT;
    }
    return 0;
}

/* stat → stat64 (i386 layout, 96 bytes) */
static long stat_out(struct task *t, uint64_t k64, uint64_t u)
{
    uint8_t a[144], b[96];
    if (in32(t, a, k64, sizeof a) < 0) return -C_EFAULT;
    uint64_t dev, ino, nlink, rdev, size, blksize, blocks, at, atn, mt, mtn, ct, ctn;
    uint32_t mode, uid, gid;
    memcpy(&dev, a, 8); memcpy(&ino, a + 8, 8); memcpy(&nlink, a + 16, 8); memcpy(&mode, a + 24, 4);
    memcpy(&uid, a + 28, 4); memcpy(&gid, a + 32, 4); memcpy(&rdev, a + 40, 8); memcpy(&size, a + 48, 8);
    memcpy(&blksize, a + 56, 8); memcpy(&blocks, a + 64, 8);
    memcpy(&at, a + 72, 8); memcpy(&atn, a + 80, 8); memcpy(&mt, a + 88, 8); memcpy(&mtn, a + 96, 8);
    memcpy(&ct, a + 104, 8); memcpy(&ctn, a + 112, 8);
    memset(b, 0, sizeof b);
    uint32_t ino32 = (uint32_t)ino, nl = (uint32_t)nlink, bs = (uint32_t)blksize;
    uint32_t t32[6] = { (uint32_t)at, (uint32_t)atn, (uint32_t)mt, (uint32_t)mtn, (uint32_t)ct, (uint32_t)ctn };
    memcpy(b, &dev, 8); memcpy(b + 12, &ino32, 4); memcpy(b + 16, &mode, 4); memcpy(b + 20, &nl, 4);
    memcpy(b + 24, &uid, 4); memcpy(b + 28, &gid, 4); memcpy(b + 32, &rdev, 8); memcpy(b + 44, &size, 8);
    memcpy(b + 52, &bs, 4); memcpy(b + 56, &blocks, 8); memcpy(b + 64, t32, 24); memcpy(b + 88, &ino, 8);
    return out32(t, u, b, sizeof b) < 0 ? -C_EFAULT : 0;
}

/* statfs → statfs64 (i386, 84 bytes) */
static long statfs_out(struct task *t, uint64_t k64, uint64_t u)
{
    uint64_t a[15];
    uint8_t b[84];
    if (in32(t, a, k64, 120) < 0) return -C_EFAULT;
    memset(b, 0, sizeof b);
    uint32_t type = (uint32_t)a[0], bsize = (uint32_t)a[1], namelen = (uint32_t)a[8], frsize = (uint32_t)a[9], flags = (uint32_t)a[10];
    memcpy(b, &type, 4); memcpy(b + 4, &bsize, 4);
    memcpy(b + 8, &a[2], 8); memcpy(b + 16, &a[3], 8); memcpy(b + 24, &a[4], 8); memcpy(b + 32, &a[5], 8); memcpy(b + 40, &a[6], 8);
    memcpy(b + 48, &a[7], 8); memcpy(b + 56, &namelen, 4); memcpy(b + 60, &frsize, 4); memcpy(b + 64, &flags, 4);
    return out32(t, u, b, sizeof b) < 0 ? -C_EFAULT : 0;
}

/* rusage: two timevals and fourteen longs */
static long rusage_out(struct task *t, uint64_t k64, uint64_t u)
{
    if (!u) return 0;
    int64_t a[18];
    int32_t b[18];
    if (in32(t, a, k64, sizeof a) < 0) return -C_EFAULT;
    for (int i = 0; i < 18; i++) b[i] = (int32_t)a[i];
    return out32(t, u, b, sizeof b) < 0 ? -C_EFAULT : 0;
}

/* siginfo: the union starts at 12 in i386, 16 in x86_64 */
static long siginfo_out(struct task *t, uint64_t k64, uint64_t u)
{
    if (!u) return 0;
    uint8_t a[128], b[128];
    if (in32(t, a, k64, sizeof a) < 0) return -C_EFAULT;
    memset(b, 0, sizeof b);
    memcpy(b, a, 12);
    memcpy(b + 12, a + 16, 112);
    return out32(t, u, b, sizeof b) < 0 ? -C_EFAULT : 0;
}

static void siginfo_build32(uint8_t *b, struct task *t, int sig)
{
    int code = t->si_code[sig - 1], value = (int)t->si_value[sig - 1], timer = t->si_timer[sig - 1];
    memset(b, 0, 128);
    memcpy(b, &sig, 4);
    memcpy(b + 8, &code, 4);
    if (code == -2) { memcpy(b + 12, &timer, 4); memcpy(b + 20, &value, 4); }        /* SI_TIMER */
    else if (code == -1) memcpy(b + 20, &value, 4);                                  /* SI_QUEUE */
    else if (code == 0 || code == -6) { memcpy(b + 12, &t->si_pid[sig - 1], 4); memcpy(b + 16, &t->si_uid[sig - 1], 4); }
}

/* ── signals ────────────────────────────────────────────────────────── */
/* struct sigcontext (i386), 88 bytes */
static void sc_save(uint32_t *sc, struct trapframe *tf, uint64_t mask, int trapno)
{
    uint16_t gs, fs;
    __asm__ volatile("movw %%gs, %0; movw %%fs, %1" : "=r"(gs), "=r"(fs));
    memset(sc, 0, 88);
    sc[0] = gs; sc[1] = fs; sc[2] = 0x1b; sc[3] = 0x1b;
    sc[4] = (uint32_t)tf->rdi; sc[5] = (uint32_t)tf->rsi; sc[6] = (uint32_t)tf->rbp; sc[7] = (uint32_t)tf->rsp;
    sc[8] = (uint32_t)tf->rbx; sc[9] = (uint32_t)tf->rdx; sc[10] = (uint32_t)tf->rcx; sc[11] = (uint32_t)tf->rax;
    sc[12] = (uint32_t)trapno; sc[13] = 0; sc[14] = (uint32_t)tf->rip; sc[15] = 0x3b;
    sc[16] = (uint32_t)tf->rflags; sc[17] = (uint32_t)tf->rsp; sc[18] = 0x1b; sc[19] = 0;
    sc[20] = (uint32_t)mask;
}

static void sc_restore(const uint32_t *sc, struct trapframe *tf)
{
    tf->rdi = sc[4]; tf->rsi = sc[5]; tf->rbp = sc[6]; tf->rsp = sc[7];
    tf->rbx = sc[8]; tf->rdx = sc[9]; tf->rcx = sc[10]; tf->rax = sc[11];
    tf->rip = sc[14]; tf->rflags = (sc[16] & 0xCD5) | 0x202;
    tf->cs = 0x3b; tf->ss = 0x1b;
    uint16_t gs = (uint16_t)sc[0];
    uint64_t tls[3];
    rs_get_tls(tls);
    rs_set_tls(tls, gs);                                     /* (a bad selector becomes 0) */
}

/* deliver sig to a 32-bit program: an rt_sigframe (SA_SIGINFO) or the old sigframe */
int compat_signal(struct task *t, struct trapframe *tf, int sig, struct sigact *a, uint64_t frame_mask)
{
    uint32_t sp = (uint32_t)tf->rsp;
    uint32_t handler = (uint32_t)a->handler, restorer = (uint32_t)a->restorer;
    if (a->flags & 4) {                                       /* SA_SIGINFO: rt frame */
        /* pretcode, sig, &info, &uc, info[128], ucontext (flags, link, stack[3], sigcontext, sigmask) */
        uint8_t f[4 * 4 + 128 + 20 + 88 + 8];
        memset(f, 0, sizeof f);
        uint32_t base = ((sp - (uint32_t)sizeof f - 64) & ~15u) - 4;    /* (esp + 4) 16-aligned at entry */
        uint32_t w[4] = { restorer, (uint32_t)sig, base + 16, base + 144 };
        memcpy(f, w, 16);
        siginfo_build32(f + 16, t, sig);
        sc_save((uint32_t *)(f + 144 + 20), tf, frame_mask, 0);
        memcpy(f + 144 + 20 + 88, &frame_mask, 8);
        if (out32(t, base, f, sizeof f) < 0) return -1;
        tf->rsp = base;
        tf->rax = (uint64_t)sig; tf->rdx = base + 16; tf->rcx = base + 144;   /* (for regparm handlers) */
    } else {
        /* pretcode, sig, sigcontext, extramask */
        uint8_t f[8 + 88 + 4];
        memset(f, 0, sizeof f);
        uint32_t base = ((sp - (uint32_t)sizeof f - 64) & ~15u) - 4;
        uint32_t w[2] = { restorer, (uint32_t)sig };
        memcpy(f, w, 8);
        sc_save((uint32_t *)(f + 8), tf, frame_mask, 0);
        uint32_t hi = (uint32_t)(frame_mask >> 32);
        memcpy(f + 96, &hi, 4);
        if (out32(t, base, f, sizeof f) < 0) return -1;
        tf->rsp = base;
        tf->rax = (uint64_t)sig; tf->rdx = 0; tf->rcx = 0;
    }
    t->si_code[sig - 1] = 0;
    tf->rip = handler;
    tf->cs = 0x3b;
    if (!(a->flags & 0x40000000)) t->sigmask |= a->mask | (1ULL << (sig - 1));   /* unless SA_NODEFER */
    if (a->flags & 0x80000000) a->handler = 0;                /* SA_RESETHAND */
    t->sigmask &= ~((1ULL << 8) | (1ULL << 18));              /* KILL, STOP */
    return 0;
}

static long compat_rt_sigreturn(struct task *t, struct trapframe *tf)
{
    uint32_t frame = (uint32_t)tf->rsp - 4;                   /* the handler's ret popped pretcode */
    uint8_t uc[20 + 88 + 8];
    if (in32(t, uc, frame + 144, sizeof uc) < 0) compat_exit(t, 11);
    sc_restore((const uint32_t *)(uc + 20), tf);
    memcpy(&t->sigmask, uc + 108, 8);
    t->sigmask &= ~((1ULL << 8) | (1ULL << 18));
    return (long)(uint32_t)tf->rax;
}

static long compat_sigreturn(struct task *t, struct trapframe *tf)
{
    uint32_t frame = (uint32_t)tf->rsp - 8;                   /* ret popped pretcode, __restore popped sig */
    uint8_t f[8 + 88 + 4];
    if (in32(t, f, frame, sizeof f) < 0) compat_exit(t, 11);
    sc_restore((const uint32_t *)(f + 8), tf);
    uint32_t lo, hi;
    memcpy(&lo, f + 8 + 80, 4);
    memcpy(&hi, f + 96, 4);
    t->sigmask = ((uint64_t)hi << 32 | lo) & ~((1ULL << 8) | (1ULL << 18));
    return (long)(uint32_t)tf->rax;
}

/* rt_sigaction: i386 struct sigaction is handler, flags, restorer (4 bytes each), mask (8) */
static long compat_rt_sigaction(struct task *t, struct trapframe *tf, struct scratch *s, uint64_t sig, uint64_t act, uint64_t oact, uint64_t size)
{
    uint64_t k = 0, ko = 0;
    if (act) {
        uint32_t a[5];
        if (in32(t, a, act, 20) < 0) return -C_EFAULT;
        uint64_t b[4] = { a[0] == 0xFFFFFFFFu ? ~0ULL : a[0], a[1], a[2], (uint64_t)a[4] << 32 | a[3] };
        k = scr_alloc(s, 32);
        if (out32(t, k, b, 32) < 0) return -C_EFAULT;
    }
    if (oact) ko = scr_alloc(s, 32);
    long r = S64(13, SX(sig), k, ko, size, 0, 0);
    if (r < 0 || !oact) return r;
    uint64_t b[4];
    if (in32(t, b, ko, 32) < 0) return -C_EFAULT;
    uint32_t a[5] = { (uint32_t)b[0], (uint32_t)b[1], (uint32_t)b[2], (uint32_t)b[3], (uint32_t)(b[3] >> 32) };
    return out32(t, oact, a, 20) < 0 ? -C_EFAULT : 0;
}

/* ── sockets: msghdr and its control messages ───────────────────────── */
static long msg_in(struct task *t, struct scratch *s, uint64_t u, uint64_t *out, uint64_t *kctl, uint32_t *uctl, uint32_t *uctllen)
{
    uint32_t m[7];
    if (in32(t, m, u, 28) < 0) return -C_EFAULT;
    uint64_t iov;
    long r = iov_in(t, s, m[2], m[3], &iov);
    if (r < 0) return r;
    /* control messages: cmsghdr is {u32 len, int level, int type} + data (4-aligned) in i386,
       {u64 len, int level, int type} + data (8-aligned) in x86_64 */
    uint64_t ctl = 0, clen = 0;
    *uctl = m[4]; *uctllen = m[5];
    if (m[4] && m[5]) {
        uint32_t n = m[5];
        uint8_t in[2048], o[4096];
        size_t on = 0;
        if (n > sizeof in) n = sizeof in;
        if (in32(t, in, m[4], n) < 0) return -C_EFAULT;
        memset(o, 0, sizeof o);
        for (uint32_t p = 0; p + 12 <= n; ) {
            uint32_t len, level, type;
            memcpy(&len, in + p, 4); memcpy(&level, in + p + 4, 4); memcpy(&type, in + p + 8, 4);
            if (len < 12 || p + len > n) break;
            uint32_t dl = len - 12;
            uint64_t l64 = 16 + dl;
            if (on + ((l64 + 7) & ~7ULL) > sizeof o) break;
            memcpy(o + on, &l64, 8); memcpy(o + on + 8, &level, 4); memcpy(o + on + 12, &type, 4);
            memcpy(o + on + 16, in + p + 12, dl);
            on += (l64 + 7) & ~7ULL;
            p += (len + 3) & ~3u;
        }
        /* receiving: room for what the kernel writes, twice the i386 size covers the wider headers */
        clen = on ? on : (uint64_t)m[5] * 2 + 16;
        if (clen > 4096) clen = 4096;
        ctl = scr_alloc(s, clen);
        if (on && out32(t, ctl, o, on) < 0) return -C_EFAULT;
    }
    uint64_t h[7] = { m[0], m[1], iov, m[3], ctl, clen, m[6] };
    *out = scr_alloc(s, 56);
    *kctl = ctl;
    return out32(t, *out, h, 56) < 0 ? -C_EFAULT : 0;
}

/* after recvmsg: namelen, flags and control messages back into the i386 msghdr */
static long msg_out(struct task *t, uint64_t k, uint64_t u, uint64_t kctl, uint32_t uctl, uint32_t uctllen)
{
    uint64_t h[7];
    if (in32(t, h, k, 56) < 0) return -C_EFAULT;
    uint32_t namelen = (uint32_t)h[1], flags = (uint32_t)h[6], outlen = 0;
    if (kctl && uctl && h[5]) {
        uint8_t in[4096], o[2048];
        size_t n = h[5] > sizeof in ? sizeof in : (size_t)h[5], on = 0;
        if (in32(t, in, kctl, n) < 0) return -C_EFAULT;
        for (size_t p = 0; p + 16 <= n; ) {
            uint64_t len;
            uint32_t level, type;
            memcpy(&len, in + p, 8); memcpy(&level, in + p + 8, 4); memcpy(&type, in + p + 12, 4);
            if (len < 16 || p + len > n) break;
            uint32_t dl = (uint32_t)len - 16, l32 = 12 + dl;
            if (on + ((l32 + 3) & ~3u) > uctllen || on + l32 > sizeof o) { flags |= 8; break; }   /* MSG_CTRUNC */
            memcpy(o + on, &l32, 4); memcpy(o + on + 4, &level, 4); memcpy(o + on + 8, &type, 4);
            memcpy(o + on + 12, in + p + 16, dl);
            on += (l32 + 3) & ~3u;
            p += (len + 7) & ~7ULL;
        }
        if (on && out32(t, uctl, o, on) < 0) return -C_EFAULT;
        outlen = (uint32_t)on;
    }
    if (out32(t, u + 4, &namelen, 4) < 0 || out32(t, u + 20, &outlen, 4) < 0 || out32(t, u + 24, &flags, 4) < 0) return -C_EFAULT;
    return 0;
}

/* ── the table of calls that only need their number changed ─────────── */
struct direct { uint16_t n32, n64; const char *sig; };     /* sig: 'i' = sign-extend that argument */
static const struct direct DIRECT[] = {
    { 1, 60, "i" }, { 2, 57, "" }, { 3, 0, "i" }, { 4, 1, "i" }, { 5, 2, "uii" }, { 6, 3, "i" },
    { 8, 85, "ui" }, { 9, 86, "" }, { 10, 87, "" }, { 11, 59, "" }, { 12, 80, "" }, { 14, 133, "uiu" },
    { 15, 90, "ui" }, { 19, 8, "iii" }, { 20, 39, "" }, { 21, 165, "" }, { 24, 102, "" }, { 29, 34, "" },
    { 33, 21, "ui" }, { 36, 162, "" }, { 37, 62, "ii" }, { 38, 82, "" }, { 39, 83, "ui" }, { 40, 84, "" },
    { 41, 32, "i" }, { 42, 22, "" }, { 45, 12, "" }, { 47, 104, "" }, { 49, 107, "" }, { 50, 108, "" },
    { 54, 16, "iu" }, { 57, 109, "ii" }, { 60, 95, "" }, { 61, 161, "" }, { 63, 33, "ii" }, { 64, 110, "" },
    { 65, 111, "" }, { 66, 112, "" }, { 83, 88, "" }, { 85, 89, "uui" }, { 91, 11, "" }, { 92, 76, "ui" },
    { 93, 77, "ii" }, { 94, 91, "ii" }, { 96, 140, "ii" }, { 97, 141, "iii" }, { 118, 74, "i" }, { 122, 63, "" },
    { 125, 10, "uui" }, { 132, 121, "i" }, { 133, 81, "i" }, { 143, 73, "ii" }, { 144, 26, "uui" }, { 147, 124, "i" },
    { 148, 75, "i" }, { 150, 149, "" }, { 151, 150, "" }, { 152, 151, "i" }, { 153, 152, "" },
    { 155, 143, "i" }, { 156, 144, "iiu" }, { 157, 145, "i" }, { 158, 24, "" }, { 159, 146, "i" }, { 160, 147, "i" },
    { 163, 25, "uuuiu" }, { 168, 7, "uui" }, { 172, 157, "i" }, { 175, 14, "iuuu" }, { 176, 127, "" },
    { 179, 130, "" }, { 183, 79, "" }, { 190, 58, "" }, { 198, 94, "uii" }, { 199, 102, "" }, { 200, 104, "" },
    { 201, 107, "" }, { 202, 108, "" }, { 203, 113, "ii" }, { 204, 114, "ii" }, { 205, 115, "iu" }, { 206, 116, "iu" },
    { 207, 93, "iii" }, { 208, 117, "iii" }, { 209, 118, "" }, { 210, 119, "iii" }, { 211, 120, "" }, { 212, 92, "uii" },
    { 213, 105, "i" }, { 214, 106, "i" }, { 215, 122, "i" }, { 216, 123, "i" }, { 217, 155, "" }, { 218, 27, "" }, { 219, 28, "uui" },
    { 220, 217, "iuu" }, { 224, 186, "" }, { 226, 188, "uuuui" }, { 227, 189, "uuuui" }, { 228, 190, "iuuui" },
    { 229, 191, "" }, { 230, 192, "" }, { 231, 193, "i" }, { 232, 194, "" }, { 233, 195, "" }, { 234, 196, "i" },
    { 235, 197, "" }, { 236, 198, "" }, { 237, 199, "i" }, { 238, 200, "ii" }, { 239, 40, "iiuu" },
    { 241, 203, "iuu" }, { 242, 204, "iuu" }, { 252, 231, "i" }, { 254, 213, "i" }, { 255, 233, "iiiu" },
    { 256, 232, "iuii" }, { 258, 218, "" }, { 262, 225, "i" }, { 263, 226, "i" }, { 270, 234, "iii" },
    { 289, 251, "iii" }, { 290, 252, "ii" }, { 291, 253, "" }, { 292, 254, "iuu" }, { 293, 255, "ii" },
    { 295, 257, "iuii" }, { 296, 258, "iui" }, { 297, 259, "iuiu" }, { 298, 260, "iuiii" }, { 301, 263, "iui" },
    { 302, 264, "iuiu" }, { 303, 265, "iuiui" }, { 304, 266, "uiu" }, { 305, 267, "iuui" }, { 306, 268, "iui" },
    { 307, 269, "iui" }, { 310, 272, "i" }, { 311, 273, "" }, { 312, 274, "i" }, { 313, 275, "iuiuui" }, { 315, 276, "iiui" },
    { 318, 309, "" }, { 319, 281, "iuiiuu" }, { 321, 282, "iuu" }, { 322, 283, "ii" }, { 323, 284, "ui" },
    { 327, 289, "iuui" }, { 328, 290, "ui" }, { 329, 291, "i" }, { 330, 292, "iii" }, { 331, 293, "ui" },
    { 332, 294, "i" }, { 340, 302, "iiuu" }, { 344, 306, "i" }, { 346, 308, "ii" }, { 349, 312, "iiiuu" },
    { 353, 316, "iuiui" }, { 355, 318, "uui" }, { 356, 319, "ui" }, { 358, 322, "iuuui" },
    { 359, 41, "iii" }, { 360, 53, "iiiu" }, { 361, 49, "iui" }, { 362, 42, "iui" }, { 363, 50, "ii" },
    { 364, 288, "iuui" }, { 367, 51, "iuu" }, { 368, 52, "iuu" }, { 369, 44, "iuuiui" }, { 371, 45, "iuuiuu" },
    { 373, 48, "ii" }, { 375, 324, "ii" }, { 376, 325, "uui" }, { 377, 326, "iuiuui" }, { 383, 332, "iuiiu" },
    { 384, 158, "i" }, { 403, 228, "i" }, { 404, 227, "i" }, { 406, 229, "i" }, { 407, 230, "iiuu" },
    { 408, 224, "i" }, { 409, 223, "iiuu" }, { 410, 287, "i" }, { 411, 286, "iiuu" }, { 412, 280, "iuui" },
    { 414, 271, "uiuuu" }, { 421, 128, "uuuu" }, { 422, 202, "uiiuui" }, { 423, 148, "i" }, { 424, 424, "iiui" },
    { 434, 434, "ii" }, { 436, 436, "iii" }, { 437, 437, "iuuu" }, { 438, 438, "iii" }, { 439, 439, "iuii" },
    { 441, 441, "iuiiuu" },
};

/* ── the dispatcher ─────────────────────────────────────────────────── */
static long compat_call(struct task *t, struct trapframe *tf, uint32_t nr, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f);

static long socketcall(struct task *t, struct trapframe *tf, uint32_t call, uint32_t uargs)
{
    static const uint8_t NARGS[21] = { 0, 3, 3, 3, 2, 3, 3, 3, 4, 4, 4, 6, 6, 2, 5, 5, 3, 3, 4, 5, 4 };
    static const uint16_t NR[21] = { 0, 359, 361, 362, 363, 0, 367, 368, 360, 0, 0, 369, 371, 373, 366, 365, 370, 372, 364, 0, 0 };
    if (call < 1 || call > 20) return -C_EINVAL;
    uint32_t v[6] = { 0 };
    if (in32(t, v, uargs, NARGS[call] * 4u) < 0) return -C_EFAULT;
    switch (call) {
    case 5: return compat_call(t, tf, 364, v[0], v[1], v[2], 0, 0, 0);                  /* accept */
    case 9: return compat_call(t, tf, 369, v[0], v[1], v[2], v[3], 0, 0);               /* send */
    case 10: return compat_call(t, tf, 371, v[0], v[1], v[2], v[3], 0, 0);              /* recv */
    }
    if (!NR[call]) return -C_ENOSYS;
    return compat_call(t, tf, NR[call], v[0], v[1], v[2], v[3], v[4], v[5]);
}

static long compat_call(struct task *t, struct trapframe *tf, uint32_t nr, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f)
{
    struct scratch s;
    scr_init(&s, tf);
    uint64_t k, k2;
    long r;
    switch (nr) {
    case 7: return S64(61, SX(a), b, SX(c), 0, 0, 0);                                    /* waitpid */
    case 13: {                                                                          /* time */
        r = S64(201, 0, 0, 0, 0, 0, 0);
        uint32_t v = (uint32_t)r;
        if (a && out32(t, a, &v, 4) < 0) return -C_EFAULT;
        return (long)v;
    }
    case 43: {                                                                          /* times */
        k = scr_alloc(&s, 32);
        r = S64(100, a ? k : 0, 0, 0, 0, 0, 0);
        if (a) { uint64_t x[4]; uint32_t y[4]; in32(t, x, k, 32); for (int i = 0; i < 4; i++) y[i] = (uint32_t)x[i]; if (out32(t, a, y, 16) < 0) return -C_EFAULT; }
        return (long)(uint32_t)r;
    }
    case 55: case 221: {                                                                /* fcntl, fcntl64 */
        uint32_t cmd = b;
        if (nr == 221 && (cmd == 12 || cmd == 13 || cmd == 14 || cmd == 36 || cmd == 37 || cmd == 38)) {
            /* struct flock64 in i386: short type, short whence, loff_t start, loff_t len, int pid (packed) */
            uint8_t l[24];
            if (in32(t, l, c, 24) < 0) return -C_EFAULT;
            uint8_t w[32];
            memset(w, 0, sizeof w);
            memcpy(w, l, 4); memcpy(w + 8, l + 4, 16); memcpy(w + 24, l + 20, 4);
            k = scr_alloc(&s, 32);
            if (out32(t, k, w, 32) < 0) return -C_EFAULT;
            uint32_t c64 = cmd >= 36 ? cmd : cmd - 7;                                   /* F_GETLK64 12 → F_GETLK 5 … */
            r = S64(72, SX(a), c64, k, 0, 0, 0);
            if (r >= 0 && (cmd == 12 || cmd == 36)) {
                in32(t, w, k, 32);
                memcpy(l, w, 4); memcpy(l + 4, w + 8, 16); memcpy(l + 20, w + 24, 4);
                if (out32(t, c, l, 24) < 0) return -C_EFAULT;
            }
            return r;
        }
        return S64(72, SX(a), cmd, (cmd == 0 || cmd == 1030) ? SX(c) : c, 0, 0, 0);    /* F_DUPFD(_CLOEXEC) take an int */
    }
    case 75: case 76: case 191: {                                                       /* setrlimit, getrlimit, ugetrlimit */
        k = scr_alloc(&s, 16);
        if (nr == 75) {
            uint32_t v[2];
            if (in32(t, v, b, 8) < 0) return -C_EFAULT;
            uint64_t w[2] = { v[0] == 0xFFFFFFFFu ? ~0ULL : v[0], v[1] == 0xFFFFFFFFu ? ~0ULL : v[1] };
            out32(t, k, w, 16);
            return S64(160, a, k, 0, 0, 0, 0);
        }
        r = S64(97, a, k, 0, 0, 0, 0);
        if (r < 0) return r;
        uint64_t w[2];
        in32(t, w, k, 16);
        uint32_t v[2] = { w[0] > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)w[0], w[1] > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)w[1] };
        return out32(t, b, v, 8) < 0 ? -C_EFAULT : 0;
    }
    case 77: k = scr_alloc(&s, 144); r = S64(98, SX(a), k, 0, 0, 0, 0); if (r < 0) return r; return rusage_out(t, k, b);
    case 78: {                                                                          /* gettimeofday */
        k = scr_alloc(&s, 16);
        r = S64(96, a ? k : 0, b, 0, 0, 0, 0);
        if (r < 0) return r;
        return ts_out(t, k, a, 1);
    }
    case 79: if ((r = ts_in(t, &s, a, 1, &k)) < 0) return r; return S64(164, k, b, 0, 0, 0, 0);   /* settimeofday */
    case 142: {                                                                         /* _newselect */
        if ((r = ts_in(t, &s, e, 1, &k)) < 0) return r;
        r = S64(23, SX(a), b, c, d, k, 0);
        if (e) ts_out(t, k, e, 1);
        return r;
    }
    case 90: {                                                                          /* old mmap: args in memory */
        uint32_t v[6];
        if (in32(t, v, a, 24) < 0) return -C_EFAULT;
        if (v[5] & 0xFFF) return -C_EINVAL;
        return compat_call(t, tf, 192, v[0], v[1], v[2], v[3], v[4], v[5] >> 12);
    }
    case 192: return S64(9, a, b, SX(c), SX(d), SX(e), (uint64_t)f << 12);              /* mmap2: offset in pages */
    case 268: case 269: {                                                               /* statfs64, fstatfs64 */
        k = scr_alloc(&s, 120);
        if (nr == 268) r = S64(137, a, k, 0, 0, 0, 0); else r = S64(138, SX(a), k, 0, 0, 0, 0);
        if (r < 0) return r;
        return statfs_out(t, k, c);
    }
    case 102: return socketcall(t, tf, a, b);
    case 114: {                                                                         /* wait4 */
        k = d ? scr_alloc(&s, 144) : 0;
        r = S64(61, SX(a), b, SX(c), k, 0, 0);
        if (r >= 0 && d) rusage_out(t, k, d);
        return r;
    }
    case 116: {                                                                         /* sysinfo */
        uint64_t v[14];
        k = scr_alloc(&s, 112);
        if ((r = S64(99, k, 0, 0, 0, 0, 0)) < 0) return r;
        in32(t, v, k, 112);
        uint32_t unit = (uint32_t)v[13] ? (uint32_t)v[13] : 1, w[16];
        uint64_t mx = v[4];
        while (mx / (unit) > 0xFFFFFFFFu) unit *= 2;
        uint32_t scale = unit / ((uint32_t)v[13] ? (uint32_t)v[13] : 1);
        memset(w, 0, sizeof w);
        w[0] = (uint32_t)v[0]; w[1] = (uint32_t)v[1]; w[2] = (uint32_t)v[2]; w[3] = (uint32_t)v[3];
        for (int i = 0; i < 6; i++) w[4 + i] = (uint32_t)(v[4 + i] / scale);
        w[10] = (uint32_t)(v[10] & 0xFFFF);
        w[13] = unit;
        return out32(t, a, w, 64) < 0 ? -C_EFAULT : 0;
    }
    case 119: return compat_sigreturn(t, tf);
    case 173: return compat_rt_sigreturn(t, tf);
    case 120: return S64(56, a, b, c, e, d, 0);                                          /* clone: (flags, sp, ptid, tls, ctid) */
    case 435: return -C_ENOSYS;                                                         /* clone3: glibc falls back to clone */
    case 140: {                                                                         /* _llseek */
        uint64_t off = (uint64_t)b << 32 | c;
        r = S64(8, SX(a), off, SX(e), 0, 0, 0);
        if (r < 0) return r;
        int64_t v = r;
        return out32(t, d, &v, 8) < 0 ? -C_EFAULT : 0;
    }
    case 145: case 146: {                                                               /* readv, writev */
        if ((r = iov_in(t, &s, b, c, &k)) < 0) return r;
        return S64(nr == 145 ? 19 : 20, SX(a), k, c, 0, 0, 0);
    }
    case 333: case 334: {                                                               /* preadv, pwritev */
        if ((r = iov_in(t, &s, b, c, &k)) < 0) return r;
        return S64(nr == 333 ? 295 : 296, SX(a), k, c, (uint64_t)e << 32 | d, 0, 0);
    }
    case 316: if ((r = iov_in(t, &s, b, c, &k)) < 0) return r; return S64(278, SX(a), k, c, d, 0, 0);   /* vmsplice */
    case 162: {                                                                         /* nanosleep */
        if ((r = ts_in(t, &s, a, 1, &k)) < 0) return r;
        k2 = b ? scr_alloc(&s, 16) : 0;
        r = S64(35, k, k2, 0, 0, 0, 0);
        if (b) ts_out(t, k2, b, 1);
        return r;
    }
    case 267: {                                                                         /* clock_nanosleep */
        if ((r = ts_in(t, &s, c, 1, &k)) < 0) return r;
        k2 = d ? scr_alloc(&s, 16) : 0;
        r = S64(230, SX(a), SX(b), k, k2, 0, 0);
        if (d) ts_out(t, k2, d, 1);
        return r;
    }
    case 264: if ((r = ts_in(t, &s, b, 1, &k)) < 0) return r; return S64(227, SX(a), k, 0, 0, 0, 0);
    case 265: case 266: {                                                               /* clock_gettime, clock_getres */
        k = scr_alloc(&s, 16);
        r = S64(nr == 265 ? 228 : 229, SX(a), b ? k : 0, 0, 0, 0, 0);
        if (r < 0) return r;
        return ts_out(t, k, b, 1);
    }
    case 174: return compat_rt_sigaction(t, tf, &s, a, b, c, d);
    case 177: {                                                                         /* rt_sigtimedwait */
        if ((r = ts_in(t, &s, c, 1, &k)) < 0) return r;
        k2 = b ? scr_alloc(&s, 128) : 0;
        r = S64(128, a, k2, k, d, 0, 0);
        if (r > 0 && b) siginfo_out(t, k2, b);
        return r;
    }
    case 178: return S64(129, SX(a), SX(b), c, 0, 0, 0);                                 /* rt_sigqueueinfo (siginfo: as given) */
    case 180: return S64(17, SX(a), b, c, (uint64_t)e << 32 | d, 0, 0);                 /* pread64 */
    case 181: return S64(18, SX(a), b, c, (uint64_t)e << 32 | d, 0, 0);                 /* pwrite64 */
    case 186: {                                                                         /* sigaltstack */
        uint64_t ka = 0, ko = 0;
        if (a) {
            uint32_t v[3];
            if (in32(t, v, a, 12) < 0) return -C_EFAULT;
            uint64_t w[3] = { v[0], v[1], v[2] };
            ka = scr_alloc(&s, 24);
            out32(t, ka, w, 24);
        }
        if (b) ko = scr_alloc(&s, 24);
        r = S64(131, ka, ko, 0, 0, 0, 0);
        if (r >= 0 && b) {
            uint64_t w[3];
            in32(t, w, ko, 24);
            uint32_t v[3] = { (uint32_t)w[0], (uint32_t)w[1], (uint32_t)w[2] };
            if (out32(t, b, v, 12) < 0) return -C_EFAULT;
        }
        return r;
    }
    case 193: return S64(76, a, (uint64_t)c << 32 | b, 0, 0, 0, 0);                    /* truncate64 */
    case 194: return S64(77, SX(a), (uint64_t)c << 32 | b, 0, 0, 0, 0);                /* ftruncate64 */
    case 195: case 196: case 197: case 300: {                                           /* stat64, lstat64, fstat64, fstatat64 */
        k = scr_alloc(&s, 144);
        if (nr == 195) r = S64(262, (uint64_t)-100, a, k, 0, 0, 0);
        else if (nr == 196) r = S64(262, (uint64_t)-100, a, k, 0x100, 0, 0);
        else if (nr == 197) r = S64(5, SX(a), k, 0, 0, 0, 0);
        else r = S64(262, SX(a), b, k, d, 0, 0);
        if (r < 0) return r;
        return stat_out(t, k, nr == 300 ? c : b);
    }
    case 225: return S64(187, SX(a), (uint64_t)c << 32 | b, d, 0, 0, 0);               /* readahead */
    case 240: {                                                                         /* futex, 32-bit timeout */
        uint32_t op = b & 127;
        if ((op == 0 || op == 9 || op == 6 || op == 11 || op == 13) && d) {
            if ((r = ts_in(t, &s, d, 1, &k)) < 0) return r;
            return S64(202, a, b, SX(c), k, e, SX(f));
        }
        return S64(202, a, b, SX(c), d, e, SX(f));
    }
    case 243: return set_thread_area(t, a);
    case 244: return get_thread_area(t, a);
    case 250: return S64(221, SX(a), (uint64_t)c << 32 | b, d, SX(e), 0, 0);           /* fadvise64 */
    case 272: return S64(221, SX(a), (uint64_t)c << 32 | b, (uint64_t)e << 32 | d, SX(f), 0, 0);   /* fadvise64_64 */
    case 324: return S64(285, SX(a), SX(b), (uint64_t)d << 32 | c, (uint64_t)f << 32 | e, 0, 0);    /* fallocate */
    case 314: return S64(277, SX(a), (uint64_t)c << 32 | b, (uint64_t)e << 32 | d, f, 0, 0);       /* sync_file_range */
    case 259: {                                                                         /* timer_create: sigevent */
        if (!b) return S64(222, SX(a), 0, c, 0, 0, 0);
        uint32_t v[16];
        if (in32(t, v, b, 64) < 0) return -C_EFAULT;
        uint8_t w[64];
        memset(w, 0, sizeof w);
        uint64_t val = v[0];
        memcpy(w, &val, 8); memcpy(w + 8, &v[1], 4); memcpy(w + 12, &v[2], 4); memcpy(w + 16, &v[3], 4);
        k = scr_alloc(&s, 64);
        out32(t, k, w, 64);
        return S64(222, SX(a), k, c, 0, 0, 0);
    }
    case 260: case 325: {                                                               /* timer_settime, timerfd_settime */
        if ((r = ts_in(t, &s, c, 2, &k)) < 0) return r;
        k2 = d ? scr_alloc(&s, 32) : 0;
        r = S64(nr == 260 ? 223 : 286, SX(a), SX(b), k, k2, 0, 0);
        if (r >= 0 && d) ts_out(t, k2, d, 2);
        return r;
    }
    case 261: case 326: {                                                               /* timer_gettime, timerfd_gettime */
        k = scr_alloc(&s, 32);
        r = S64(nr == 261 ? 224 : 287, SX(a), k, 0, 0, 0, 0);
        if (r < 0) return r;
        return ts_out(t, k, b, 2);
    }
    case 271: case 299: {                                                               /* utimes, futimesat */
        uint64_t path = nr == 271 ? a : b, tv = nr == 271 ? b : c;
        if ((r = ts_in(t, &s, tv, 2, &k)) < 0) return r;
        return nr == 271 ? S64(235, path, k, 0, 0, 0, 0) : S64(261, SX(a), path, k, 0, 0, 0);
    }
    case 320: if ((r = ts_in(t, &s, c, 2, &k)) < 0) return r; return S64(280, SX(a), b, k, SX(d), 0, 0);   /* utimensat */
    case 284: {                                                                         /* waitid */
        k = c ? scr_alloc(&s, 128) : 0;
        k2 = e ? scr_alloc(&s, 144) : 0;
        r = S64(247, SX(a), SX(b), k, SX(d), k2, 0);
        if (r >= 0) { if (c) siginfo_out(t, k, c); if (e) rusage_out(t, k2, e); }
        return r;
    }
    case 308: case 413: {                                                               /* pselect6 (+ time64) */
        if (nr == 308) { if ((r = ts_in(t, &s, e, 1, &k)) < 0) return r; }
        else k = e;
        uint64_t ks = 0;
        if (f) {
            uint32_t v[2];
            if (in32(t, v, f, 8) < 0) return -C_EFAULT;
            uint64_t w[2] = { v[0], v[1] };
            ks = scr_alloc(&s, 16);
            out32(t, ks, w, 16);
        }
        r = S64(270, SX(a), b, c, d, k, ks);
        if (nr == 308 && e) ts_out(t, k, e, 1);
        return r;
    }
    case 309: if ((r = ts_in(t, &s, c, 1, &k)) < 0) return r; return S64(271, a, b, k, d, e, 0);   /* ppoll */
    case 354: {                                                                         /* seccomp: sock_fprog */
        if (a == 1 && c) {
            uint32_t v[2];
            if (in32(t, v, c, 8) < 0) return -C_EFAULT;
            uint64_t w[2] = { v[0] & 0xFFFF, v[1] };
            k = scr_alloc(&s, 16);
            out32(t, k, w, 16);
            return S64(317, a, b, k, 0, 0, 0);
        }
        return S64(317, a, b, c, 0, 0, 0);
    }
    case 365: case 366: {                                                               /* getsockopt, setsockopt */
        uint32_t lvl = b, opt = c;
        if (lvl == 1 && (opt == 20 || opt == 21)) {                                     /* SO_RCVTIMEO / SNDTIMEO, old 32-bit timeval */
            if (nr == 366) { if ((r = ts_in(t, &s, d, 1, &k)) < 0) return r; return S64(54, SX(a), 1, opt, k, 16, 0); }
            k = scr_alloc(&s, 16); k2 = scr_alloc(&s, 8);
            uint32_t len = 16;
            out32(t, k2, &len, 4);
            r = S64(55, SX(a), 1, opt, k, k2, 0);
            if (r < 0) return r;
            len = 8;
            if (out32(t, e, &len, 4) < 0) return -C_EFAULT;
            return ts_out(t, k, d, 1);
        }
        if (lvl == 1 && (opt == 66 || opt == 67)) opt = opt == 66 ? 20 : 21;            /* the _NEW ones are 64-bit already */
        return nr == 365 ? S64(55, SX(a), SX(lvl), opt, d, e, 0) : S64(54, SX(a), SX(lvl), opt, d, e, 0);
    }
    case 370: {                                                                         /* sendmsg */
        uint32_t uc, ul;
        if ((r = msg_in(t, &s, b, &k, &k2, &uc, &ul)) < 0) return r;
        return S64(46, SX(a), k, c, 0, 0, 0);
    }
    case 372: {                                                                         /* recvmsg */
        uint32_t uc, ul;
        if ((r = msg_in(t, &s, b, &k, &k2, &uc, &ul)) < 0) return r;
        r = S64(47, SX(a), k, c, 0, 0, 0);
        if (r >= 0) { long e2 = msg_out(t, k, b, k2, uc, ul); if (e2 < 0) return e2; }
        return r;
    }
    }
    /* everything else: the same call under its x86_64 number */
    for (size_t i = 0; i < sizeof DIRECT / sizeof *DIRECT; i++) {
        if (DIRECT[i].n32 != nr) continue;
        uint32_t in[6] = { a, b, c, d, e, f };
        uint64_t o[6];
        const char *sg = DIRECT[i].sig;
        size_t sl = strlen(sg);
        for (int j = 0; j < 6; j++) o[j] = (size_t)j < sl && sg[j] == 'i' ? SX(in[j]) : in[j];
        return syscall_call(t, tf, DIRECT[i].n64, o[0], o[1], o[2], o[3], o[4], o[5]);
    }
    return -C_ENOSYS;
}

int sys_trace_compat;

/* int $0x80 from a 32-bit program */
void compat_syscall(struct trapframe *tf)
{
    __asm__ volatile("sti");
    struct task *t = task_current();
    if (!t) { tf->rax = (uint64_t)-C_ENOSYS; return; }
    t->tf = tf;
    uint32_t nr = (uint32_t)tf->rax, a = (uint32_t)tf->rbx, b = (uint32_t)tf->rcx, c = (uint32_t)tf->rdx,
             d = (uint32_t)tf->rsi, e = (uint32_t)tf->rdi, f = (uint32_t)tf->rbp;
    long r = 0;
    int run = 1;
    if (t->ns.seccomp_mode) {
        uint64_t args[6] = { a, b, c, d, e, f };
        int k = seccomp_check_arch(t, (long)nr, args, tf->rip, &r, 1);
        if (k == 2 || k == 3) compat_exit(t, 31);                                       /* SIGSYS */
        run = k == 1;
    }
    if (run) r = compat_call(t, tf, nr, a, b, c, d, e, f);
    if (sys_trace_compat) kprintf("[%d %s] i386 %u(%x, %x, %x) = %ld\n", t->pid, t->name, nr, a, b, c, r);
    tf->rax = (uint64_t)(uint32_t)r;
    uproc_signals(t, tf);
}
