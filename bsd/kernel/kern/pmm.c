/* Physical memory manager: bitmap of 4 KiB frames covering the first 4 GiB. */
#include "kern.h"

#define MAX_FRAMES (1UL << 20)
#define ALIGN_UP(x, a)   (((x) + (a) - 1) & ~((a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((a) - 1))

extern char __kernel_start[], __kernel_end[];

static uint32_t bitmap[MAX_FRAMES / 32];   /* 1 = used */
static size_t   free_count, total_count;
static size_t   hint = 256;                /* never hand out < 1 MiB */

static inline int  is_used(size_t f) { return bitmap[f >> 5] & (1u << (f & 31)); }
static inline void set_used(size_t f) { if (!is_used(f)) { bitmap[f >> 5] |= 1u << (f & 31); free_count--; } }
static inline void set_free(size_t f) { if (is_used(f)) { bitmap[f >> 5] &= ~(1u << (f & 31)); free_count++; } }

static void range_free(uint64_t base, uint64_t len)
{
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
}

uint64_t pmm_alloc_page(void)
{
    for (size_t n = 0; n < MAX_FRAMES; n++) {
        size_t f = (hint + n) % MAX_FRAMES;
        if (!is_used(f)) { set_used(f); hint = f + 1; return (uint64_t)f * PAGE_SIZE; }
    }
    return 0;
}

uint64_t pmm_alloc_contig(size_t pages)
{
    size_t run = 0;
    for (size_t f = 256; f < MAX_FRAMES; f++) {
        run = is_used(f) ? 0 : run + 1;
        if (run == pages) {
            size_t start = f + 1 - pages;
            for (size_t i = 0; i < pages; i++) set_used(start + i);
            return (uint64_t)start * PAGE_SIZE;
        }
    }
    return 0;
}

void   pmm_free_page(uint64_t a) { set_free(a / PAGE_SIZE); }
size_t pmm_free_pages(void)  { return free_count; }
size_t pmm_total_pages(void) { return total_count; }
