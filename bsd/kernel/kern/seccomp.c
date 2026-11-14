/* seccomp: system-call filters, as Linux has them. A filter is a classic BPF program run on
 * every system call with the call's number, architecture, address and arguments; it answers
 * allow, fail with an errno, send SIGSYS, or kill. Filters stack (the strictest answer wins),
 * are inherited by children and kept across execve, and can't be removed. WebKit's sandbox
 * (through bubblewrap) installs one that refuses mount, unshare, ptrace and the like. */
#include "kern.h"
#include "arch.h"
#include "rs.h"
#include "uproc.h"
#include "ns.h"

enum { EACCES = 13, EFAULT = 14, EINVAL = 22, ENOMEM = 12, ENOSYS = 38, EOPNOTSUPP = 95 };

#define AUDIT_ARCH_X86_64 0xC000003Eu

#define RET_KILL_PROCESS 0x80000000u
#define RET_KILL_THREAD  0x00000000u
#define RET_TRAP         0x00030000u
#define RET_ERRNO        0x00050000u
#define RET_USER_NOTIF   0x7FC00000u
#define RET_TRACE        0x7FF00000u
#define RET_LOG          0x7FFC0000u
#define RET_ALLOW        0x7FFF0000u
#define RET_ACTION_FULL  0xFFFF0000u
#define RET_DATA         0x0000FFFFu

/* classic BPF */
#define BPF_CLASS(c) ((c) & 0x07)
#define BPF_LD 0x00
#define BPF_LDX 0x01
#define BPF_ST 0x02
#define BPF_STX 0x03
#define BPF_ALU 0x04
#define BPF_JMP 0x05
#define BPF_RET 0x06
#define BPF_MISC 0x07
#define BPF_SIZE(c) ((c) & 0x18)
#define BPF_W 0x00
#define BPF_MODE(c) ((c) & 0xe0)
#define BPF_IMM 0x00
#define BPF_ABS 0x20
#define BPF_MEM 0x60
#define BPF_LEN 0x80
#define BPF_OP(c) ((c) & 0xf0)
#define BPF_SRC(c) ((c) & 0x08)
#define BPF_K 0x00
#define BPF_X 0x08
#define BPF_A 0x10

struct insn { uint16_t code; uint8_t jt, jf; uint32_t k; };

void seccomp_put(struct seccomp_filter *f)
{
    while (f && --f->refs == 0) {
        struct seccomp_filter *p = f->prev;
        rs_kfree(f);
        f = p;
    }
}

/* the checks Linux makes before it accepts a program (seccomp_check_filter + sk_chk_filter) */
static int check(const struct insn *p, uint32_t len)
{
    if (!len || len > 4096) return -EINVAL;
    for (uint32_t pc = 0; pc < len; pc++) {
        const struct insn *i = &p[pc];
        uint16_t c = i->code;
        switch (BPF_CLASS(c)) {
        case BPF_LD:
            if (BPF_MODE(c) == BPF_ABS) { if (BPF_SIZE(c) != BPF_W || (i->k & 3) || i->k >= 64) return -EINVAL; }
            else if (BPF_MODE(c) == BPF_LEN) { if (BPF_SIZE(c) != BPF_W) return -EINVAL; }
            else if (BPF_MODE(c) == BPF_MEM) { if (i->k >= 16) return -EINVAL; }
            else if (BPF_MODE(c) != BPF_IMM) return -EINVAL;
            break;
        case BPF_LDX:
            if (BPF_MODE(c) == BPF_MEM) { if (i->k >= 16) return -EINVAL; }
            else if (BPF_MODE(c) != BPF_IMM && BPF_MODE(c) != BPF_LEN) return -EINVAL;
            break;
        case BPF_ST: case BPF_STX:
            if (i->k >= 16) return -EINVAL;
            break;
        case BPF_ALU:
            switch (BPF_OP(c)) {
            case 0x00: case 0x10: case 0x20: case 0x40: case 0x50: case 0x60: case 0x70: case 0x80: case 0x90: case 0xa0: break;
            case 0x30: if (BPF_SRC(c) == BPF_K && i->k == 0) return -EINVAL; break;      /* DIV by 0 */
            default: return -EINVAL;
            }
            break;
        case BPF_JMP:
            if (BPF_OP(c) == 0x00) { if (i->k >= len - pc - 1) return -EINVAL; }          /* JA */
            else if (BPF_OP(c) == 0x10 || BPF_OP(c) == 0x20 || BPF_OP(c) == 0x30 || BPF_OP(c) == 0x40) {
                if ((uint32_t)i->jt >= len - pc - 1 || (uint32_t)i->jf >= len - pc - 1) return -EINVAL;
            } else return -EINVAL;
            break;
        case BPF_RET:
            if ((c & 0x18) != BPF_K && (c & 0x18) != BPF_A) return -EINVAL;
            break;
        case BPF_MISC:
            if (c != (BPF_MISC | 0x00) && c != (BPF_MISC | 0x80)) return -EINVAL;      /* TAX, TXA */
            break;
        }
    }
    if (BPF_CLASS(p[len - 1].code) != BPF_RET) return -EINVAL;
    return 0;
}

static uint32_t run(const struct insn *p, uint32_t len, const uint8_t *data)
{
    uint32_t A = 0, X = 0, M[16] = { 0 };
    for (uint32_t pc = 0; pc < len; pc++) {
        const struct insn *i = &p[pc];
        uint16_t c = i->code;
        switch (BPF_CLASS(c)) {
        case BPF_LD:
            if (BPF_MODE(c) == BPF_ABS) memcpy(&A, data + i->k, 4);
            else if (BPF_MODE(c) == BPF_LEN) A = 64;
            else if (BPF_MODE(c) == BPF_MEM) A = M[i->k];
            else A = i->k;
            break;
        case BPF_LDX:
            if (BPF_MODE(c) == BPF_MEM) X = M[i->k];
            else if (BPF_MODE(c) == BPF_LEN) X = 64;
            else X = i->k;
            break;
        case BPF_ST: M[i->k] = A; break;
        case BPF_STX: M[i->k] = X; break;
        case BPF_ALU: {
            uint32_t v = BPF_SRC(c) == BPF_X ? X : i->k;
            switch (BPF_OP(c)) {
            case 0x00: A += v; break;
            case 0x10: A -= v; break;
            case 0x20: A *= v; break;
            case 0x30: if (!v) return RET_KILL_PROCESS; A /= v; break;
            case 0x40: A |= v; break;
            case 0x50: A &= v; break;
            case 0x60: A = v >= 32 ? 0 : A << v; break;
            case 0x70: A = v >= 32 ? 0 : A >> v; break;
            case 0x80: A = -A; break;
            case 0x90: if (!v) return RET_KILL_PROCESS; A %= v; break;
            case 0xa0: A ^= v; break;
            }
            break;
        }
        case BPF_JMP: {
            uint32_t v = BPF_SRC(c) == BPF_X ? X : i->k;
            int take;
            switch (BPF_OP(c)) {
            case 0x00: pc += i->k; continue;
            case 0x10: take = A == v; break;
            case 0x20: take = A > v; break;
            case 0x30: take = A >= v; break;
            default: take = (A & v) != 0; break;
            }
            pc += take ? i->jt : i->jf;
            break;
        }
        case BPF_RET: return (c & 0x18) == BPF_A ? A : i->k;
        case BPF_MISC: if (c & 0x80) A = X; else X = A; break;
        }
    }
    return RET_KILL_PROCESS;                                  /* can't happen: check() wants a final RET */
}

long seccomp_set_filter(struct task *t, uint64_t ufprog, long flags)
{
    if (flags & ~(1L | 2L | 4L | 16L)) return -EINVAL;      /* TSYNC, LOG, SPEC_ALLOW, TSYNC_ESRCH; no listener */
    if (!t->ns.no_new_privs && !ns_capable(t, CAP_SYS_ADMIN)) return -EACCES;
    struct { uint16_t len; uint16_t pad[3]; uint64_t filter; } prog;
    if (copyin(t->mm, &prog, ufprog, 16) < 0) return -EFAULT;
    if (!prog.len || prog.len > 4096) return -EINVAL;
    struct seccomp_filter *f = rs_kmalloc(sizeof *f + (size_t)prog.len * 8);
    if (!f) return -ENOMEM;
    f->refs = 1; f->len = prog.len;
    if (copyin(t->mm, f->insns, prog.filter, (size_t)prog.len * 8) < 0) { rs_kfree(f); return -EFAULT; }
    long r = check((const struct insn *)f->insns, f->len);
    if (r < 0) { rs_kfree(f); return r; }
    f->prev = t->ns.filter;                                   /* our reference to the old chain moves to f */
    t->ns.filter = f;
    t->ns.seccomp_mode = 2;
    if (flags & 1)                                            /* TSYNC: every thread of the process */
        for (int i = 0; i < MAXTASK; i++) {
            struct task *o = task_slot(i);
            if (!o->used || o == t || o->tgid != t->tgid) continue;
            struct seccomp_filter *old = o->ns.filter;
            f->refs++;
            o->ns.filter = f;
            o->ns.seccomp_mode = 2;
            seccomp_put(old);
        }
    return 0;
}

long sys_seccomp(struct task *t, long op, long flags, uint64_t uargs)
{
    switch (op) {
    case 0:                                                   /* SECCOMP_SET_MODE_STRICT */
        if (flags || uargs) return -EINVAL;
        if (t->ns.seccomp_mode == 2) return -EINVAL;
        t->ns.seccomp_mode = 1;
        return 0;
    case 1:                                                   /* SECCOMP_SET_MODE_FILTER */
        if (t->ns.seccomp_mode == 1) return -EINVAL;
        return seccomp_set_filter(t, uargs, flags);
    case 2: {                                                 /* SECCOMP_GET_ACTION_AVAIL */
        uint32_t a;
        if (flags) return -EINVAL;
        if (copyin(t->mm, &a, uargs, 4) < 0) return -EFAULT;
        switch (a) {
        case RET_KILL_PROCESS: case RET_KILL_THREAD: case RET_TRAP: case RET_ERRNO: case RET_TRACE: case RET_LOG: case RET_ALLOW: return 0;
        }
        return -EOPNOTSUPP;
    }
    }
    return -EINVAL;                                           /* GET_NOTIF_SIZES: no user notification */
}

/* 1: go ahead with the call; 0: don't, *ret is its result; 2: kill the process; 3: kill the thread */
int seccomp_check_arch(struct task *t, long nr, const uint64_t args[6], uint64_t ip, long *ret, int compat);

int seccomp_check(struct task *t, long nr, const uint64_t args[6], uint64_t ip, long *ret)
{
    return seccomp_check_arch(t, nr, args, ip, ret, 0);
}

/* compat: an i386 system call (its own numbers, AUDIT_ARCH_I386), so a filter written for
 * x86_64 sees a foreign architecture, as on Linux, rather than misreading the numbers */
int seccomp_check_arch(struct task *t, long nr, const uint64_t args[6], uint64_t ip, long *ret, int compat)
{
    if (t->ns.seccomp_mode == 1) {                            /* strict: read, write, exit, sigreturn */
        if (!compat && (nr == 0 || nr == 1 || nr == 60 || nr == 15)) return 1;
        if (compat && (nr == 3 || nr == 4 || nr == 1 || nr == 119 || nr == 173)) return 1;
        return 2;
    }
    struct seccomp_filter *f = t->ns.filter;
    if (!f) return 1;
    uint8_t data[64];
    int32_t n32 = (int32_t)nr;
    uint32_t arch = compat ? 0x40000003u : AUDIT_ARCH_X86_64;
    memcpy(data, &n32, 4); memcpy(data + 4, &arch, 4); memcpy(data + 8, &ip, 8);
    memcpy(data + 16, args, 48);
    uint32_t r = RET_ALLOW;
    for (; f; f = f->prev) {
        uint32_t v = run((const struct insn *)f->insns, f->len, data);
        if ((int32_t)(v & RET_ACTION_FULL) < (int32_t)(r & RET_ACTION_FULL)) r = v;
    }
    switch (r & RET_ACTION_FULL) {
    case RET_ALLOW: case RET_LOG: return 1;
    case RET_ERRNO: { uint32_t e = r & RET_DATA; if (e > 4095) e = 4095; *ret = -(long)e; return 0; }
    case RET_TRAP: t->pending |= 1ULL << 30; *ret = -ENOSYS; return 0;      /* SIGSYS */
    case RET_TRACE: case RET_USER_NOTIF: *ret = -ENOSYS; return 0;
    case RET_KILL_THREAD: return 3;
    default: return 2;
    }
}
