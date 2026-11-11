/* execve: load an x86_64 ELF (static or dynamically linked through its PT_INTERP, PIE or not)
 * or a #! script into a fresh address space, and build the initial stack the Linux ABI
 * specifies: argc, argv, envp, the auxiliary vector, and the strings. */
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

static int check(struct node *n, struct ehdr *e)
{
    if (n->size < sizeof *e) return -8;                  /* ENOEXEC */
    memcpy(e, n->data, sizeof *e);
    if (memcmp(e->ident, "\x7f" "ELF", 4) || e->ident[4] != 2 || e->machine != 62) return -8;
    if (e->type != 2 && e->type != 3) return -8;
    if (e->phoff + (uint64_t)e->phnum * sizeof(struct phdr) > n->size) return -8;
    return 0;
}

/* map the PT_LOAD segments; returns the load bias */
static int load_segments(struct mm *m, struct node *n, const struct ehdr *e, uint64_t bias, uint64_t *end)
{
    const struct phdr *ph = (const struct phdr *)(n->data + e->phoff);
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
    struct node *n;
    int r = fs_lookup(t->cwd, path, 1, &n);
    if (r < 0) return r;
    if (n->type == N_DIR) return -13;
    if (n->type != N_REG) return -13;
    if (!(n->mode & 0111)) return -13;                   /* EACCES */

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
    if ((r = check(n, &e)) < 0) return r;
    struct mm *m = mm_new();
    if (!m) return -12;
    uint64_t bias = e.type == 3 ? PIE_BASE : 0, end = 0;
    if ((r = load_segments(m, n, &e, bias, &end)) < 0) { mm_put(m); return r; }
    m->brk_start = m->brk = (end + 0x10000) & ~(PAGE_SIZE - 1);

    uint64_t entry = bias + e.entry, interp_base = 0, phdr_va = 0;
    const struct phdr *ph = (const struct phdr *)(n->data + e.phoff);
    for (int i = 0; i < e.phnum; i++) {
        if (ph[i].type == PT_PHDR) phdr_va = bias + ph[i].vaddr;
        if (ph[i].type == PT_LOAD && !phdr_va && ph[i].off <= e.phoff && e.phoff < ph[i].off + ph[i].filesz)
            phdr_va = bias + ph[i].vaddr + (e.phoff - ph[i].off);
        if (ph[i].type == PT_INTERP) {
            char ip[256];
            size_t l = ph[i].filesz < sizeof ip - 1 ? (size_t)ph[i].filesz : sizeof ip - 1;
            memcpy(ip, n->data + ph[i].off, l);
            ip[l] = 0;
            struct node *in;
            struct ehdr ie;
            uint64_t iend;
            if ((r = fs_lookup(0, ip, 1, &in)) < 0 || (r = check(in, &ie)) < 0) { mm_put(m); return r < 0 ? r : -8; }
            interp_base = INTERP_BASE - ((in->size + 0x200000) & ~0xFFFFFULL);
            if ((r = load_segments(m, in, &ie, interp_base, &iend)) < 0) { mm_put(m); return r; }
            entry = interp_base + ie.entry;
        }
    }

    /* stack */
    if ((r = mm_map(m, STACK_TOP - STACK_SIZE, STACK_SIZE, VM_READ | VM_WRITE, 0, 0, 0, 0)) < 0) { mm_put(m); return r; }
    uint64_t sp = STACK_TOP;
    int argc = 0, envc = 0;
    while (kargv[argc]) argc++;
    while (kenvp && kenvp[envc]) envc++;
    if (argc > 4096 || envc > 4096) { mm_put(m); return -7; }
    uint64_t *uargv = rs_kmalloc(sizeof(uint64_t) * (size_t)(argc + envc + 2));
    if (!uargv) { mm_put(m); return -12; }
    const char *execfn = path;
    char platform[] = "x86_64";
    uint8_t rnd[16];
    for (int i = 0; i < 16; i++) rnd[i] = (uint8_t)(rdtsc() >> (i % 8));
    r = push(m, &sp, execfn, strlen(execfn) + 1);
    uint64_t execfn_va = sp;
    r |= push(m, &sp, platform, sizeof platform);
    uint64_t plat_va = sp;
    r |= push(m, &sp, rnd, 16);
    uint64_t rnd_va = sp;
    for (int i = envc - 1; i >= 0; i--) { r |= push(m, &sp, kenvp[i], strlen(kenvp[i]) + 1); uargv[argc + 1 + i] = sp; }
    for (int i = argc - 1; i >= 0; i--) { r |= push(m, &sp, kargv[i], strlen(kargv[i]) + 1); uargv[i] = sp; }
    sp &= ~15ULL;
    uint64_t aux[] = {
        3, phdr_va, 4, sizeof(struct phdr), 5, e.phnum, 6, PAGE_SIZE, 7, interp_base, 8, 0,
        9, bias + e.entry, 11, 0, 12, 0, 13, 0, 14, 0, 16, 0x078bfbff, 17, AUXO_HZ, 23, 0,
        25, rnd_va, 31, execfn_va, 15, plat_va, 0, 0
    };
    size_t words = 1 + (size_t)argc + 1 + (size_t)envc + 1 + sizeof aux / 8;
    if (words & 1) sp -= 8;                                /* keep rsp 16-byte aligned at entry */
    r |= push(m, &sp, aux, sizeof aux);
    uint64_t z = 0;
    r |= push(m, &sp, &z, 8);
    for (int i = envc - 1; i >= 0; i--) r |= push(m, &sp, &uargv[argc + 1 + i], 8);
    r |= push(m, &sp, &z, 8);
    for (int i = argc - 1; i >= 0; i--) r |= push(m, &sp, &uargv[i], 8);
    uint64_t ac = (uint64_t)argc;
    r |= push(m, &sp, &ac, 8);
    rs_kfree(uargv);
    if (r) { mm_put(m); return -14; }

    /* commit: the old address space goes, this one comes */
    struct mm *old = t->mm;
    t->mm = m;
    rs_set_ucr3(m->pml4);
    if (old) mm_put(old);
    const char *base = strrchr(path, '/');
    strlcpy(t->name, base ? base + 1 : path, sizeof t->name);
    fs_path(n, t->exe, sizeof t->exe);
    memset(tf, 0, sizeof *tf);
    tf->rip = entry;
    tf->rsp = sp;
    tf->cs = 0x23;
    tf->ss = 0x1b;
    tf->rflags = 0x202;
    __asm__ volatile("wrmsr" :: "c"(0xC0000100), "a"(0), "d"(0));   /* no TLS until the program sets one */
    return 0;
}
