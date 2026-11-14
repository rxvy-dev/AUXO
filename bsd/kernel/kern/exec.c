/* execve: load an x86_64 or i386 ELF (static or dynamically linked through its PT_INTERP, PIE
 * or not) or a #! script into a fresh address space, and build the initial stack the Linux ABI
 * specifies: argc, argv, envp, the auxiliary vector, and the strings. A 32-bit program runs in
 * compatibility mode (CS 0x3b) with everything below 4 GB; its system calls go through
 * compat32.c. */
#include "kern.h"
#include "rs.h"
#include "fs.h"
#include "uproc.h"

struct ehdr { uint8_t ident[16]; uint16_t type, machine; uint32_t version; uint64_t entry, phoff, shoff;
              uint32_t flags; uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; };
struct phdr { uint32_t type, flags; uint64_t off, vaddr, paddr, filesz, memsz, align; };

#define PT_LOAD 1
#define PT_INTERP 3
#define PT_PHDR 6
#define PT_GNU_STACK 0x6474e551
#define PIE_BASE  0x0000555555554000ULL
#define INTERP_BASE 0x00007FFFF7FC0000ULL
#define STACK_TOP 0x00007FFFFFFFE000ULL
#define STACK_SIZE (8ULL << 20)
#define PIE_BASE32    0x56555000ULL
#define INTERP_BASE32 0xF7F00000ULL
#define STACK_TOP32   0xFFFFD000ULL

struct ehdr32 { uint8_t ident[16]; uint16_t type, machine; uint32_t version, entry, phoff, shoff, flags;
                uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; };
struct phdr32 { uint32_t type, off, vaddr, paddr, filesz, memsz, flags, align; };

/* an ELF this machine runs: x86_64, or i386 (*is32). Its program headers come back in the
 * 64-bit layout either way, in ph (rs_kfree it). */
static int check(struct node *n, struct ehdr *e, struct phdr **ph, int *is32)
{
    *ph = 0;
    if (n->size < sizeof(struct ehdr32) || memcmp(n->data, "\x7f" "ELF", 4)) return -8;   /* ENOEXEC */
    if (n->data[4] == 1) {
        struct ehdr32 h;
        memcpy(&h, n->data, sizeof h);
        if (h.machine != 3 || (h.type != 2 && h.type != 3)) return -8;
        if ((uint64_t)h.phoff + (uint64_t)h.phnum * sizeof(struct phdr32) > n->size || h.phnum > 128) return -8;
        memset(e, 0, sizeof *e);
        memcpy(e->ident, h.ident, 16);
        e->type = h.type; e->machine = h.machine; e->entry = h.entry; e->phoff = h.phoff; e->phnum = h.phnum;
        *ph = rs_kmalloc(sizeof(struct phdr) * (h.phnum ? h.phnum : 1));
        if (!*ph) return -12;
        for (int i = 0; i < h.phnum; i++) {
            struct phdr32 q;
            memcpy(&q, n->data + h.phoff + (uint64_t)i * sizeof q, sizeof q);
            (*ph)[i] = (struct phdr){ q.type, q.flags, q.off, q.vaddr, q.paddr, q.filesz, q.memsz, q.align };
        }
        *is32 = 1;
        return 0;
    }
    if (n->size < sizeof *e) return -8;
    memcpy(e, n->data, sizeof *e);
    if (e->ident[4] != 2 || e->machine != 62) return -8;
    if (e->type != 2 && e->type != 3) return -8;
    if (e->phoff + (uint64_t)e->phnum * sizeof(struct phdr) > n->size || e->phnum > 128) return -8;
    *ph = rs_kmalloc(sizeof(struct phdr) * (e->phnum ? e->phnum : 1));
    if (!*ph) return -12;
    memcpy(*ph, n->data + e->phoff, sizeof(struct phdr) * e->phnum);
    *is32 = 0;
    return 0;
}

/* map the PT_LOAD segments; returns the load bias */
static int load_segments(struct mm *m, struct node *n, const struct ehdr *e, const struct phdr *ph, uint64_t bias, uint64_t *end)
{
    uint64_t hi = 0;
    for (int i = 0; i < e->phnum; i++) {
        if (ph[i].type != PT_LOAD || !ph[i].memsz) continue;
        uint32_t prot = (ph[i].flags & 4 ? VM_READ : 0) | (ph[i].flags & 2 ? VM_WRITE : 0) | (ph[i].flags & 1 ? VM_EXEC : 0);
        uint64_t va = bias + ph[i].vaddr, page = va & ~(PAGE_SIZE - 1), lead = va - page;
        uint64_t fileend = va + ph[i].filesz, memend = va + ph[i].memsz;
        /* the file part (with the zero tail of its last page), then any anonymous rest (.bss) */
        uint64_t fmap_end = (fileend + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        if (ph[i].filesz) {
            int r = mm_map(m, page, fmap_end - page, prot, 0, n, ph[i].off - lead, ph[i].filesz + lead);
            if (r < 0) return r;
        } else fmap_end = page;
        uint64_t aend = (memend + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        if (aend > fmap_end) { int r = mm_map(m, fmap_end, aend - fmap_end, prot, 0, 0, 0, 0); if (r < 0) return r; }
        if (aend > hi) hi = aend;
    }
    *end = hi;
    return 0;
}

static int push(struct mm *m, uint64_t *sp, const void *data, size_t n)
{
    *sp -= n;
    return copyout(m, *sp, data, n);
}

int exec_load(struct task *t, const char *path, char *const kargv[], char *const kenvp[], struct trapframe *tf)
{
    struct path at;
    int r = vfs_walk(t, t->ns.cwd, path, 1, &at);
    if (r < 0) return r;
    struct node *n = at.n;
    if (at.m && (at.m->flags & MNT_NOEXEC)) return -13;   /* a noexec mount (/proc, /dev …) */
    if (fs_load(n) < 0) return -5;
    if (n->type == N_DIR) return -13;
    if (n->type != N_REG) return -13;
    if (!(n->mode & 0111)) return -13;                   /* EACCES */
    if (cred_perm(t, n, 1) < 0) return -13;

    /* #! scripts: run the interpreter with the script as its argument */
    if (n->size > 2 && n->data[0] == '#' && n->data[1] == '!') {
        char line[256];
        size_t i = 2, o = 0;
        while (i < n->size && n->data[i] == ' ') i++;
        while (i < n->size && n->data[i] != '\n' && o + 1 < sizeof line) line[o++] = (char)n->data[i++];
        line[o] = 0;
        while (o && (line[o - 1] == ' ' || line[o - 1] == '\r')) line[--o] = 0;
        char *arg = strchr(line, ' ');
        if (arg) { *arg++ = 0; while (*arg == ' ') arg++; if (!*arg) arg = 0; }
        char *nargv[64];
        int k = 0;
        nargv[k++] = line;
        if (arg) nargv[k++] = arg;
        nargv[k++] = (char *)path;
        for (int j = 1; kargv[j] && k < 63; j++) nargv[k++] = kargv[j];
        nargv[k] = 0;
        return exec_load(t, line, nargv, kenvp, tf);
    }

    struct ehdr e;
    struct phdr *ph;
    int is32;
    if ((r = check(n, &e, &ph, &is32)) < 0) return r;
    struct cred_exec ce;                                 /* set-user-ID / set-group-ID programs */
    cred_exec_prepare(t, n, at.m && (at.m->flags & MNT_NOSUID), &ce);
    struct mm *m = mm_new();
    if (!m) { rs_kfree(ph); return -12; }
    m->compat = is32;
    uint64_t bias = e.type == 3 ? (is32 ? PIE_BASE32 : PIE_BASE) : 0, end = 0;
    if ((r = load_segments(m, n, &e, ph, bias, &end)) < 0) goto fail;
    m->brk_start = m->brk = (end + 0x10000) & ~(PAGE_SIZE - 1);

    uint64_t entry = bias + e.entry, interp_base = 0, phdr_va = 0;
    for (int i = 0; i < e.phnum; i++) {
        if (ph[i].type == PT_PHDR) phdr_va = bias + ph[i].vaddr;
        if (ph[i].type == PT_LOAD && !phdr_va && ph[i].off <= e.phoff && e.phoff < ph[i].off + ph[i].filesz)
            phdr_va = bias + ph[i].vaddr + (e.phoff - ph[i].off);
        if (ph[i].type == PT_INTERP) {
            char ip[256];
            size_t l = ph[i].filesz < sizeof ip - 1 ? (size_t)ph[i].filesz : sizeof ip - 1;
            if (ph[i].off + l > n->size) { r = -8; goto fail; }
            memcpy(ip, n->data + ph[i].off, l);
            ip[l] = 0;
            struct path ipath;
            struct ehdr ie;
            struct phdr *iph;
            int i32;
            uint64_t iend;
            if ((r = vfs_walk(t, t->ns.root, ip, 1, &ipath)) < 0) goto fail;     /* in this task's namespace */
            struct node *in = ipath.n;
            if (fs_load(in) < 0) { r = -5; goto fail; }
            if ((r = check(in, &ie, &iph, &i32)) < 0) goto fail;
            if (i32 != is32) { rs_kfree(iph); r = -8; goto fail; }      /* a 64-bit ld.so for a 32-bit program */
            uint64_t ib = is32 ? INTERP_BASE32 : INTERP_BASE;
            interp_base = ib - ((in->size + 0x200000) & ~0xFFFFFULL);
            r = load_segments(m, in, &ie, iph, interp_base, &iend);
            rs_kfree(iph);
            if (r < 0) goto fail;
            entry = interp_base + ie.entry;
        }
    }

    /* stack: words are 4 bytes for a 32-bit program, 8 for a 64-bit one */
    uint64_t stop = is32 ? STACK_TOP32 : STACK_TOP;
    size_t ws = is32 ? 4 : 8;
    if ((r = mm_map(m, stop - STACK_SIZE, STACK_SIZE, VM_READ | VM_WRITE, 0, 0, 0, 0)) < 0) goto fail;
    uint64_t sp = stop;
    int argc = 0, envc = 0;
    while (kargv[argc]) argc++;
    while (kenvp && kenvp[envc]) envc++;
    if (argc > 4096 || envc > 4096) { r = -7; goto fail; }
    uint64_t *uargv = rs_kmalloc(sizeof(uint64_t) * (size_t)(argc + envc + 2));
    if (!uargv) { r = -12; goto fail; }
    const char *execfn = path;
    const char *platform = is32 ? "i686" : "x86_64";
    uint8_t rnd[16];
    for (int i = 0; i < 16; i++) rnd[i] = (uint8_t)(rdtsc() >> (i % 8));
    r = push(m, &sp, execfn, strlen(execfn) + 1);
    uint64_t execfn_va = sp;
    r |= push(m, &sp, platform, strlen(platform) + 1);
    uint64_t plat_va = sp;
    r |= push(m, &sp, rnd, 16);
    uint64_t rnd_va = sp;
    for (int i = envc - 1; i >= 0; i--) { r |= push(m, &sp, kenvp[i], strlen(kenvp[i]) + 1); uargv[argc + 1 + i] = sp; }
    for (int i = argc - 1; i >= 0; i--) { r |= push(m, &sp, kargv[i], strlen(kargv[i]) + 1); uargv[i] = sp; }
    sp &= ~15ULL;
    uint64_t aux[] = {
        3, phdr_va, 4, is32 ? sizeof(struct phdr32) : sizeof(struct phdr), 5, e.phnum, 6, PAGE_SIZE, 7, interp_base, 8, 0,
        9, bias + e.entry, 11, t->ns.cred.uid, 12, ce.euid, 13, t->ns.cred.gid, 14, ce.egid, 16, 0x078bfbff, 17, AUXO_HZ, 23, (uint64_t)ce.secure,
        25, rnd_va, 31, execfn_va, 15, plat_va, 0, 0
    };
    size_t naux = sizeof aux / 8;
    size_t words = 1 + (size_t)argc + 1 + (size_t)envc + 1 + naux;
    while (((sp - words * ws) & 15) != 0) sp -= ws;        /* the stack pointer 16-byte aligned at entry */
    for (size_t i = naux; i-- > 0; ) r |= push(m, &sp, &aux[i], ws);
    uint64_t z = 0;
    r |= push(m, &sp, &z, ws);
    for (int i = envc - 1; i >= 0; i--) r |= push(m, &sp, &uargv[argc + 1 + i], ws);
    r |= push(m, &sp, &z, ws);
    for (int i = argc - 1; i >= 0; i--) r |= push(m, &sp, &uargv[i], ws);
    uint64_t ac = (uint64_t)argc;
    r |= push(m, &sp, &ac, ws);
    rs_kfree(uargv);
    if (r) { r = -14; goto fail; }
    rs_kfree(ph);
    ph = 0;

    /* commit: the old address space goes, this one comes */
    struct mm *old = t->mm;
    t->mm = m;
    rs_set_ucr3(m->pml4);
    if (old) mm_put(old);
    const char *base = strrchr(path, '/');
    strlcpy(t->name, base ? base + 1 : path, sizeof t->name);
    vfs_pathname(t, at, t->exe, sizeof t->exe);
    t->ns.exe = at;
    ns_exec(t);
    cred_exec_commit(t, &ce);
    memset(tf, 0, sizeof *tf);
    tf->rip = entry;
    tf->rsp = sp;
    tf->cs = is32 ? 0x3b : 0x23;
    tf->ss = 0x1b;
    tf->rflags = 0x202;
    __asm__ volatile("wrmsr" :: "c"(0xC0000100), "a"(0), "d"(0));   /* no TLS until the program sets one */
    uint64_t none[3] = { 0, 0, 0 };
    rs_set_tls(none, 0);
    return 0;
fail:
    if (ph) rs_kfree(ph);
    mm_put(m);
    return r;
}
