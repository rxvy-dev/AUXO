/* Physical memory manager: a bitmap of 4 KiB frames, up to 128 GiB.
 *
 * Memory below 4 GiB is what pmm_alloc_page/pmm_alloc_contig hand out, as they always have: devices
 * that can only address 32 bits (and the boot page tables) are safe with it. RAM above 4 GiB is
 * mapped by pmm_map_high (2 MiB pages, RAM only, never a PCI hole) and is what the _any
 * allocators prefer: programs' memory and file contents, which are most of what's used. */
#include "kern.h"
#include "arch.h"

#define MAX_FRAMES (1UL << 25)                     /* 128 GiB */
#define LOW_FRAMES (1UL << 20)                     /* 4 GiB */
#define ALIGN_UP(x, a)   (((x) + (a) - 1) & ~((a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((a) - 1))

extern char __kernel_start[], __kernel_end[];

static uint32_t bitmap[MAX_FRAMES / 32];   /* 1 = used */
static size_t   free_count, total_count;
static size_t   hint = 256;                /* never hand out < 1 MiB */
static size_t   hint_hi = LOW_FRAMES, top_frame = LOW_FRAMES;
static struct { uint64_t base, end; } high[32];   /* RAM above 4 GiB, 2 MiB aligned */
static int nhigh;

static inline int  is_used(size_t f) { return bitmap[f >> 5] & (1u << (f & 31)); }
static inline void set_used(size_t f) { if (!is_used(f)) { bitmap[f >> 5] |= 1u << (f & 31); free_count--; } }
static inline void set_free(size_t f) { if (is_used(f)) { bitmap[f >> 5] &= ~(1u << (f & 31)); free_count++; } }

static void range_free(uint64_t base, uint64_t len)
{
    uint64_t lo4 = 1ULL << 32;
    if (base + len > lo4) {                       /* the part above 4 GiB: whole 2 MiB pages only */
        uint64_t hs = ALIGN_UP(base > lo4 ? base : lo4, 0x200000ULL), he = ALIGN_DOWN(base + len, 0x200000ULL);
        if (he > (uint64_t)MAX_FRAMES * PAGE_SIZE) he = (uint64_t)MAX_FRAMES * PAGE_SIZE;
        if (he > hs && nhigh < 32) {
            high[nhigh].base = hs; high[nhigh].end = he; nhigh++;
            for (uint64_t a = hs; a < he; a += PAGE_SIZE) { set_free(a / PAGE_SIZE); total_count++; }
            if (he / PAGE_SIZE > top_frame) top_frame = he / PAGE_SIZE;
        }
        if (base >= lo4) return;
        len = lo4 - base;
    }
    uint64_t s = ALIGN_UP(base, PAGE_SIZE), e = ALIGN_DOWN(base + len, PAGE_SIZE);
    if (e > (uint64_t)MAX_FRAMES * PAGE_SIZE) e = (uint64_t)MAX_FRAMES * PAGE_SIZE;
    for (uint64_t a = s; a < e; a += PAGE_SIZE) { set_free(a / PAGE_SIZE); total_count++; }
}

static void range_used(uint64_t base, uint64_t len)
{
    uint64_t s = ALIGN_DOWN(base, PAGE_SIZE), e = ALIGN_UP(base + len, PAGE_SIZE);
    if (e > (uint64_t)MAX_FRAMES * PAGE_SIZE) e = (uint64_t)MAX_FRAMES * PAGE_SIZE;
    for (uint64_t a = s; a < e; a += PAGE_SIZE) set_used(a / PAGE_SIZE);
}

struct mb2_tag  { uint32_t type, size; };
struct mb2_mmap { uint32_t type, size, entry_size, entry_version; };
struct mb2_ent  { uint64_t base, len; uint32_t type, reserved; };

static int up;
int pmm_ready(void) { return up; }

void pmm_init(uint32_t mbi_addr)
{
    for (size_t i = 0; i < MAX_FRAMES / 32; i++) bitmap[i] = 0xFFFFFFFFu;
    free_count = 0;

    const uint8_t *mbi = (const uint8_t *)(uintptr_t)mbi_addr;
    uint32_t total = *(const uint32_t *)mbi;

    for (const uint8_t *p = mbi + 8; p < mbi + total; ) {
        const struct mb2_tag *t = (const struct mb2_tag *)p;
        if (t->type == 0) break;
        if (t->type == 6) {
            const struct mb2_mmap *m = (const struct mb2_mmap *)p;
            for (const uint8_t *e = p + sizeof *m; e < p + t->size; e += m->entry_size) {
                const struct mb2_ent *en = (const struct mb2_ent *)e;
                if (en->type == 1) range_free(en->base, en->len);
            }
        }
        p += ALIGN_UP(t->size, 8);
    }

    range_used(0, 0x100000);                                    /* legacy low 1 MiB */
    range_used((uint64_t)(uintptr_t)__kernel_start,
               (uint64_t)(uintptr_t)(__kernel_end - __kernel_start));
    range_used(mbi_addr, total);                                /* boot info */
    for (const uint8_t *p = mbi + 8; p < mbi + total; ) {        /* modules (the install image) */
        const struct mb2_tag *t = (const struct mb2_tag *)p;
        if (t->type == 0) break;
        if (t->type == 3) {
            uint32_t start = *(const uint32_t *)(p + 8), end = *(const uint32_t *)(p + 12);
            range_used(start, end - start);
        }
        p += ALIGN_UP(t->size, 8);
    }
    up = 1;
}

/* threads allocate too: keep the bitmap consistent with interrupts off */
static uint64_t irq_off(void) { uint64_t f; __asm__ volatile("pushfq; pop %0; cli" : "=r"(f) :: "memory"); return f; }
static void irq_restore(uint64_t f) { if (f & 0x200) __asm__ volatile("sti" ::: "memory"); }

uint64_t pmm_alloc_page(void)
{
    uint64_t fl = irq_off();
    for (size_t n = 0; n < LOW_FRAMES; n++) {
        size_t f = (hint + n) % LOW_FRAMES;
        if (!is_used(f)) { set_used(f); hint = f + 1; irq_restore(fl); return (uint64_t)f * PAGE_SIZE; }
    }
    irq_restore(fl);
    return 0;
}

uint64_t pmm_alloc_contig(size_t pages)
{
    size_t run = 0;
    uint64_t fl = irq_off();
    for (size_t f = 256; f < LOW_FRAMES; f++) {
        run = is_used(f) ? 0 : run + 1;
        if (run == pages) {
            size_t start = f + 1 - pages;
            for (size_t i = 0; i < pages; i++) set_used(start + i);
            irq_restore(fl);
            return (uint64_t)start * PAGE_SIZE;
        }
    }
    irq_restore(fl);
    return 0;
}

/* memory above 4 GiB when there is some (programs, file data); below otherwise */
static int high_mapped;

uint64_t pmm_alloc_page_any(void)
{
    if (high_mapped && top_frame > LOW_FRAMES) {
        uint64_t fl = irq_off();
        size_t span = top_frame - LOW_FRAMES;
        for (size_t n = 0; n < span; n++) {
            size_t f = LOW_FRAMES + (hint_hi - LOW_FRAMES + n) % span;
            if (!is_used(f)) { set_used(f); hint_hi = f + 1; irq_restore(fl); return (uint64_t)f * PAGE_SIZE; }
        }
        irq_restore(fl);
    }
    return pmm_alloc_page();
}

uint64_t pmm_alloc_contig_any(size_t pages)
{
    if (high_mapped && top_frame > LOW_FRAMES) {
        size_t run = 0;
        uint64_t fl = irq_off();
        for (size_t f = LOW_FRAMES; f < top_frame; f++) {
            run = is_used(f) ? 0 : run + 1;
            if (run == pages) {
                size_t start = f + 1 - pages;
                for (size_t i = 0; i < pages; i++) set_used(start + i);
                irq_restore(fl);
                return (uint64_t)start * PAGE_SIZE;
            }
        }
        irq_restore(fl);
    }
    return pmm_alloc_contig(pages);
}

/* extend an allocation in place: the pages right after it, if they're free */
int pmm_grow(uint64_t addr, size_t oldpages, size_t newpages)
{
    size_t f0 = (size_t)(addr / PAGE_SIZE);
    if (newpages <= oldpages) return 0;
    size_t lim = f0 < LOW_FRAMES ? LOW_FRAMES : top_frame;    /* stay in the zone it's in */
    if (f0 + newpages > lim) return -1;
    uint64_t fl = irq_off();
    for (size_t f = f0 + oldpages; f < f0 + newpages; f++) if (is_used(f)) { irq_restore(fl); return -1; }
    for (size_t f = f0 + oldpages; f < f0 + newpages; f++) set_used(f);
    irq_restore(fl);
    return 0;
}

/* identity-map the RAM above 4 GiB (after pmm_init: page directories come from low memory) */
uint64_t pmm_map_high(void)
{
    uint64_t mapped = 0;
    for (int i = 0; i < nhigh; i++) {
        if (vm_map_ram(high[i].base, high[i].end - high[i].base) < 0) {
            for (uint64_t a = high[i].base; a < high[i].end; a += PAGE_SIZE) set_used(a / PAGE_SIZE);   /* unusable */
            continue;
        }
        mapped += high[i].end - high[i].base;
    }
    high_mapped = 1;
    return mapped;
}

void   pmm_free_page(uint64_t a) { uint64_t fl = irq_off(); set_free(a / PAGE_SIZE); irq_restore(fl); }
size_t pmm_free_pages(void)  { return free_count; }
size_t pmm_total_pages(void) { return total_count; }
