/* Device memory above 4 GiB. boot.S identity-maps the low 4 GiB with 2 MiB pages; PCI BARs and
 * framebuffers on real hardware (a big NVIDIA card with Resizable BAR, say) can sit higher, so
 * map them on demand: PDPT[n] → a fresh page directory of 2 MiB pages, still identity. */
#include "kern.h"
#include "arch.h"

extern uint64_t boot_pdpt[512];

/* 2 MiB mappings anywhere. Firmware puts 64-bit device memory (GPU framebuffers, xHCI and NVMe
 * registers) wherever it likes: above 512 GiB is common with UEFI, terabytes up on some boards.
 * Below 64 TiB it's mapped at the same address (identity); higher than that isn't a valid
 * x86-64 address, so it goes into a window of its own. Page tables come from a small static
 * pool until the page allocator is up (the framebuffer is mapped before that). */
extern uint64_t pml4[512];
static uint64_t early_tables[8][512] __attribute__((aligned(4096)));
static int early_used;
#define HIGH_WINDOW (1ULL << 46)                        /* 64 TiB: device memory above it lands here */
static uint64_t window_next = HIGH_WINDOW;

static uint64_t *new_table(void)
{
    uint64_t *t = 0;
    if (pmm_ready()) { uint64_t p = pmm_alloc_page(); t = (uint64_t *)(uintptr_t)p; }
    else if (early_used < 8) t = early_tables[early_used++];
    if (t) memset(t, 0, PAGE_SIZE);
    return t;
}

static int map_2m(uint64_t va, uint64_t pa, uint64_t attr)
{
    uint64_t *l4 = &pml4[(va >> 39) & 511];
    if (!(*l4 & 1)) { uint64_t *t = new_table(); if (!t) return -1; *l4 = (uint64_t)(uintptr_t)t | 3; }
    uint64_t *pdpt = (uint64_t *)(uintptr_t)(*l4 & 0x000FFFFFFFFFF000ULL);
    uint64_t *l3 = &pdpt[(va >> 30) & 511];
    if (!(*l3 & 1)) { uint64_t *t = new_table(); if (!t) return -1; *l3 = (uint64_t)(uintptr_t)t | 3; }
    if (*l3 & 0x80) return 0;                                         /* a 1 GiB page is there already */
    uint64_t *pd = (uint64_t *)(uintptr_t)(*l3 & 0x000FFFFFFFFFF000ULL);
    pd[(va >> 21) & 511] = (pa & ~0x1FFFFFULL) | 0x83 | attr;
    __asm__ volatile("invlpg (%0)" :: "r"(va) : "memory");
    return 0;
}

static void *map_range(uint64_t phys, uint64_t len, uint64_t attr)
{
    if (!len) return 0;
    uint64_t base = phys & ~0x1FFFFFULL, end = (phys + len + 0x1FFFFF) & ~0x1FFFFFULL;
    uint64_t va = base;
    if (end > HIGH_WINDOW) {                                          /* not addressable as is */
        va = window_next;
        window_next += end - base;
        if (window_next > (1ULL << 47)) return 0;
    }
    for (uint64_t a = base; a < end; a += 0x200000)
        if (map_2m(va + (a - base), a, attr) < 0) return 0;
    return (void *)(uintptr_t)(va + (phys - base));
}

void *vm_map_mmio(uint64_t phys, uint64_t len, int uncached)
{
    if (!len) return 0;
    if (phys + len <= 0x100000000ULL) return (void *)(uintptr_t)phys;   /* already there */
    return map_range(phys, len, uncached ? 0x18 : 0);
}

/* PAT: entry 1 (PWT alone) becomes write-combining. A framebuffer mapped write-combining is
 * many times faster to draw into than uncached, which is what the firmware's MTRRs give the
 * PCI window; WC in the PAT wins over UC from an MTRR. Index 3 (PCD|PWT) stays UC for MMIO. */
static int pat_ok;
void pat_init(void)
{
    uint32_t r[4];
    cpuid(1, r);
    if (!(r[3] & (1u << 16))) return;
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0x277));
    lo = (lo & ~0xFF00u) | 0x0100u;                                  /* PA1 = WC (01) */
    __asm__ volatile("wbinvd" ::: "memory");
    __asm__ volatile("wrmsr" :: "a"(lo), "d"(hi), "c"(0x277));
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0; mov %0, %%cr3" : "=r"(cr3) :: "memory");
    __asm__ volatile("wbinvd" ::: "memory");
    pat_ok = 1;
}

/* The framebuffer, write-combining (below 4 GiB too: the boot map makes that window uncached) */
void *vm_map_fb(uint64_t phys, uint64_t len)
{
    return map_range(phys, len, pat_ok ? 0x08 : 0x18);                /* PWT = WC, else UC */
}

/* RAM above 4 GiB, identity-mapped write-back with 2 MiB pages (base and len 2 MiB aligned) */
int vm_map_ram(uint64_t base, uint64_t len)
{
    for (uint64_t a = base; a < base + len; a += 0x200000) {
        if (map_2m(a, a, 0) < 0) return -1;                       /* present, writable, 2 MiB */
    }
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0; mov %0, %%cr3" : "=r"(cr3) :: "memory");
    return 0;
}
