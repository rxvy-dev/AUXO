/* User address spaces: 4-level page tables per process, mapping records (VMAs), pages
 * allocated on first touch, and safe copying to and from user memory.
 *
 * Kernel code never runs on these page tables (see arch/x86_64/entry.S), so the kernel reaches
 * user memory by walking them: every page-table page and user page is ordinary physical
 * memory below 4 GiB, which the kernel sees at the same address. */
#include "kern.h"
#include "rs.h"
#include "fs.h"
#include "uproc.h"

#define P   1ULL
#define RW  2ULL
#define US  4ULL
#define SHR (1ULL << 9)            /* not ours to free (entry section, shared mappings) */
#define ADDR(e) ((e) & 0x000FFFFFFFFFF000ULL)

extern char __entry_start[], __entry_end[];

void mm_lock(struct mm *m)
{
    int me = rs_current_pid();
    for (;;) {
        uint64_t f;
        __asm__ volatile("pushfq; pop %0; cli" : "=r"(f) :: "memory");
        if (!m->lock_depth || m->lock_owner == me) { m->lock_owner = me; m->lock_depth++; if (f & 0x200) __asm__ volatile("sti"); return; }
        if (f & 0x200) __asm__ volatile("sti");
        rs_yield();
    }
}

void mm_unlock(struct mm *m)
{
    uint64_t f;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(f) :: "memory");
    if (--m->lock_depth == 0) m->lock_owner = 0;
    if (f & 0x200) __asm__ volatile("sti");
}

static uint64_t *table(uint64_t e) { return (uint64_t *)(uintptr_t)ADDR(e); }

static uint64_t zpage(void)
{
    uint64_t p = pmm_alloc_page_any();
    if (p) memset((void *)(uintptr_t)p, 0, PAGE_SIZE);
    return p;
}

/* the PTE for va, creating tables on the way when `create` */
static uint64_t *pte(struct mm *m, uint64_t va, int create)
{
    uint64_t *t = (uint64_t *)(uintptr_t)m->pml4;
    for (int lvl = 3; lvl > 0; lvl--) {
        int i = (int)((va >> (12 + 9 * lvl)) & 511);
        if (!(t[i] & P)) {
            if (!create) return 0;
            uint64_t nt = zpage();
            if (!nt) return 0;
            t[i] = nt | P | RW | US;
        }
        t = table(t[i]);
    }
    return &t[(va >> 12) & 511];
}

static uint64_t prot_bits(uint32_t prot)
{
    uint64_t b = P;
    if (prot & VM_WRITE) b |= RW;
    if (prot) b |= US;             /* PROT_NONE: present for the kernel, not for the program */
    return b;
}

struct mm *mm_new(void)
{
    struct mm *m = rs_kmalloc(sizeof *m);
    if (!m) return 0;
    memset(m, 0, sizeof *m);
    m->pml4 = zpage();
    if (!m->pml4) { rs_kfree(m); return 0; }
    m->refs = 1;
    /* the entry section, kernel-only, at its own address */
    for (uint64_t a = (uintptr_t)__entry_start; a < (uintptr_t)__entry_end; a += PAGE_SIZE) {
        uint64_t *e = pte(m, a, 1);
        if (e) *e = a | P | RW | SHR;
    }
    return m;
}

struct vma *mm_vma_at(struct mm *m, uint64_t a)
{
    int lo = 0, hi = m->nv - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (a < m->v[mid].start) hi = mid - 1;
        else if (a >= m->v[mid].end) lo = mid + 1;
        else return &m->v[mid];
    }
    return 0;
}

static int vma_insert(struct mm *m, const struct vma *v)
{
    if (m->nv == m->cap) {
        int nc = m->cap ? m->cap * 2 : 32;
        struct vma *nvv = rs_kmalloc((size_t)nc * sizeof *nvv);
        if (!nvv) return -12;
        if (m->nv) memcpy(nvv, m->v, (size_t)m->nv * sizeof *nvv);
        if (m->v) rs_kfree(m->v);
        m->v = nvv; m->cap = nc;
    }
    int i = m->nv;
    while (i > 0 && m->v[i - 1].start > v->start) { m->v[i] = m->v[i - 1]; i--; }
    m->v[i] = *v;
    m->nv++;
    if (v->file) { v->file->refs++; if (v->counted) v->file->maps++; }
    return 0;
}

static void free_range(struct mm *m, uint64_t s, uint64_t e)
{
    for (uint64_t a = s; a < e; a += PAGE_SIZE) {
        uint64_t *p = pte(m, a, 0);
        if (!p || !(*p & P)) continue;
        if (!(*p & SHR)) pmm_free_page(ADDR(*p));
        *p = 0;
        __asm__ volatile("invlpg (%0)" :: "r"(a) : "memory");
    }
}

/* remove [s, e) from the VMA list, splitting records that straddle it */
static int carve(struct mm *m, uint64_t s, uint64_t e)
{
    for (int i = 0; i < m->nv; i++) {
        struct vma *v = &m->v[i];
        if (v->end <= s || v->start >= e) continue;
        if (v->start < s && v->end > e) {                    /* hole in the middle: split */
            struct vma right = *v;
            right.off += e - v->start;
            right.flen = right.flen > e - v->start ? right.flen - (e - v->start) : 0;
            right.start = e;
            v->end = s;
            if (vma_insert(m, &right) < 0) return -12;
            continue;
        }
        if (v->start < s) { v->end = s; continue; }
        if (v->end > e) {
            v->off += e - v->start;
            v->flen = v->flen > e - v->start ? v->flen - (e - v->start) : 0;
            v->start = e;
            continue;
        }
        if (v->file) { if (v->counted) v->file->maps--; fs_put(v->file); }   /* wholly inside: drop it */
        memmove(v, v + 1, (size_t)(m->nv - i - 1) * sizeof *v);
        m->nv--;
        i--;
    }
    return 0;
}

static int mm_unmap_l(struct mm *m, uint64_t s, uint64_t len)
{
    uint64_t e = (s + len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    s &= ~(PAGE_SIZE - 1);
    free_range(m, s, e);
    return carve(m, s, e);
}

static int mm_map_l(struct mm *m, uint64_t s, uint64_t len, uint32_t prot, int shared, struct node *file, uint64_t off, uint64_t flen)
{
    uint64_t e = (s + len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    s &= ~(PAGE_SIZE - 1);
    if (e > USER_TOP || e <= s) return -12;
    mm_unmap_l(m, s, e - s);
    struct vma v = { s, e, prot, (uint32_t)shared, file, off, flen, 0 };
    return vma_insert(m, &v);
}

static int mm_protect_l(struct mm *m, uint64_t s, uint64_t len, uint32_t prot)
{
    uint64_t e = (s + len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    s &= ~(PAGE_SIZE - 1);
    /* split at the edges, then retag */
    for (int i = 0; i < m->nv; i++) {
        struct vma *v = &m->v[i];
        if (v->end <= s || v->start >= e) continue;
        if (v->start < s) {
            struct vma r = *v;
            r.off += s - v->start; r.flen = r.flen > s - v->start ? r.flen - (s - v->start) : 0; r.start = s;
            v->end = s;
            if (vma_insert(m, &r) < 0) return -12;
            continue;
        }
        if (v->end > e) {
            struct vma r = *v;
            r.off += e - v->start; r.flen = r.flen > e - v->start ? r.flen - (e - v->start) : 0; r.start = e;
            v->end = e;
            if (vma_insert(m, &r) < 0) return -12;
            v = &m->v[i];
        }
        v->prot = prot;
        for (uint64_t a = v->start; a < v->end; a += PAGE_SIZE) {
            uint64_t *p = pte(m, a, 0);
            if (p && (*p & P)) { *p = ADDR(*p) | (*p & SHR) | prot_bits(prot); __asm__ volatile("invlpg (%0)" :: "r"(a) : "memory"); }
        }
    }
    return 0;
}

uint64_t mm_find_free(struct mm *m, uint64_t len, uint64_t below)
{
    len = (len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint64_t top = below ? below : MMAP_TOP;
    for (int i = m->nv - 1; i >= -1; i--) {
        uint64_t gap_end = top, gap_start = i >= 0 ? m->v[i].end : 0x10000;
        if (i + 1 < m->nv && m->v[i + 1].start < gap_end) gap_end = m->v[i + 1].start;
        if (gap_end > top) gap_end = top;
        if (gap_start < gap_end && gap_end - gap_start >= len && gap_end - len >= 0x10000) return gap_end - len;
    }
    return 0;
}

static int mm_fault_l(struct mm *m, uint64_t addr, int write)
{
    struct vma *v = mm_vma_at(m, addr);
    if (!v || !v->prot) return -14;
    if (write && !(v->prot & VM_WRITE)) return -14;
    uint64_t page = addr & ~(PAGE_SIZE - 1);
    uint64_t *p = pte(m, page, 1);
    if (!p) return -12;
    if (*p & P) {                                    /* present: protection said no */
        if (write && !(*p & RW)) return -14;
        if (!(*p & US)) return -14;
        return 0;
    }
    uint64_t rel = page - v->start;
    if (v->shared && v->file) {
        /* shared file mapping (memfd, framebuffer-style buffers): the file's own pages */
        struct node *f = v->file;
        uint64_t need = v->off + (v->end - v->start);
        if ((!f->cap || f->cap < need) && fs_reserve(f, need) < 0) return -12;
        *p = ((uint64_t)(uintptr_t)f->data + v->off + rel) | SHR | prot_bits(v->prot);
        if (!v->counted) { v->counted = 1; f->maps++; }
        return 0;
    }
    uint64_t pg = zpage();
    if (!pg) return -12;
    if (v->file && rel < v->flen) {
        uint64_t fo = v->off + rel, n = v->flen - rel;
        if (n > PAGE_SIZE) n = PAGE_SIZE;
        if (fo < v->file->size) {
            if (fo + n > v->file->size) n = v->file->size - fo;
            memcpy((void *)(uintptr_t)pg, v->file->data + fo, n);
        }
    }
    *p = pg | prot_bits(v->prot);
    return 0;
}

int mm_unmap(struct mm *m, uint64_t s, uint64_t len) { mm_lock(m); int r = mm_unmap_l(m, s, len); mm_unlock(m); return r; }
int mm_map(struct mm *m, uint64_t s, uint64_t len, uint32_t prot, int shared, struct node *file, uint64_t off, uint64_t flen)
{ mm_lock(m); int r = mm_map_l(m, s, len, prot, shared, file, off, flen); mm_unlock(m); return r; }
int mm_protect(struct mm *m, uint64_t s, uint64_t len, uint32_t prot) { mm_lock(m); int r = mm_protect_l(m, s, len, prot); mm_unlock(m); return r; }
int mm_fault(struct mm *m, uint64_t addr, int write) { mm_lock(m); int r = mm_fault_l(m, addr, write); mm_unlock(m); return r; }

struct mm *mm_clone(struct mm *src)
{
    mm_lock(src);
    struct mm *r = 0;
    extern struct mm *mm_clone_l(struct mm *);
    r = mm_clone_l(src);
    mm_unlock(src);
    return r;
}

struct mm *mm_clone_l(struct mm *src)
{
    struct mm *m = mm_new();
    if (!m) return 0;
    m->brk_start = src->brk_start; m->brk = src->brk;
    for (int i = 0; i < src->nv; i++) {
        struct vma *v = &src->v[i];
        if (vma_insert(m, v) < 0) { mm_put(m); return 0; }
        for (uint64_t a = v->start; a < v->end; a += PAGE_SIZE) {
            uint64_t *sp = pte(src, a, 0);
            if (!sp || !(*sp & P)) continue;
            uint64_t *dp = pte(m, a, 1);
            if (!dp) { mm_put(m); return 0; }
            if (*sp & SHR) { *dp = *sp; continue; }
            uint64_t pg = pmm_alloc_page_any();
            if (!pg) { mm_put(m); return 0; }
            memcpy((void *)(uintptr_t)pg, (void *)(uintptr_t)ADDR(*sp), PAGE_SIZE);
            *dp = pg | (*sp & 0xFFF);
        }
    }
    return m;
}

static void free_tables(uint64_t *t, int lvl)
{
    for (int i = 0; i < 512; i++) {
        if (!(t[i] & P)) continue;
        if (lvl > 1) free_tables(table(t[i]), lvl - 1);
        else if (!(t[i] & SHR)) pmm_free_page(ADDR(t[i]));
    }
    if (lvl < 4) pmm_free_page((uint64_t)(uintptr_t)t);
}

void mm_put(struct mm *m)
{
    if (!m || --m->refs > 0) return;
    for (int i = 0; i < m->nv; i++) if (m->v[i].file) { if (m->v[i].counted) m->v[i].file->maps--; fs_put(m->v[i].file); }
    uint64_t *pml4 = (uint64_t *)(uintptr_t)m->pml4;
    for (int i = 0; i < 256; i++) if (pml4[i] & P) free_tables(table(pml4[i]), 3);
    pmm_free_page(m->pml4);
    if (m->v) rs_kfree(m->v);
    rs_kfree(m);
}

/* ── user memory access ─────────────────────────────────────────────── */
static void *uaddr(struct mm *m, uint64_t a, int write)
{
    if (a >= USER_TOP || a < 0x1000) return 0;
    uint64_t *p = pte(m, a, 0);
    if (!p || !(*p & P) || !(*p & US) || (write && !(*p & RW))) {
        if (mm_fault_l(m, a, write) < 0) return 0;
        p = pte(m, a, 0);
        if (!p || !(*p & US) || (write && !(*p & RW))) return 0;
    }
    return (void *)(uintptr_t)(ADDR(*p) + (a & (PAGE_SIZE - 1)));
}

int copyin(struct mm *m, void *dst, uint64_t src, size_t n)
{
    uint8_t *d = dst;
    mm_lock(m);
    while (n) {
        void *s = uaddr(m, src, 0);
        if (!s) { mm_unlock(m); return -14; }
        size_t chunk = PAGE_SIZE - (src & (PAGE_SIZE - 1));
        if (chunk > n) chunk = n;
        memcpy(d, s, chunk);
        d += chunk; src += chunk; n -= chunk;
    }
    mm_unlock(m);
    return 0;
}

int copyout(struct mm *m, uint64_t dst, const void *src, size_t n)
{
    const uint8_t *s = src;
    mm_lock(m);
    while (n) {
        void *d = uaddr(m, dst, 1);
        if (!d) { mm_unlock(m); return -14; }
        size_t chunk = PAGE_SIZE - (dst & (PAGE_SIZE - 1));
        if (chunk > n) chunk = n;
        memcpy(d, s, chunk);
        s += chunk; dst += chunk; n -= chunk;
    }
    mm_unlock(m);
    return 0;
}

long copyin_str(struct mm *m, char *dst, uint64_t src, size_t cap)
{
    mm_lock(m);
    for (size_t i = 0; i < cap; i++) {
        char *s = uaddr(m, src + i, 0);
        if (!s) { mm_unlock(m); return -14; }
        dst[i] = *s;
        if (!*s) { mm_unlock(m); return (long)i; }
    }
    mm_unlock(m);
    return -36;                                       /* ENAMETOOLONG */
}
