/* Device memory above 4 GiB. boot.S identity-maps the low 4 GiB with 2 MiB pages; PCI BARs and
 * framebuffers on real hardware (a big NVIDIA card with Resizable BAR, say) can sit higher, so
 * map them on demand: PDPT[n] → a fresh page directory of 2 MiB pages, still identity. */
#include "kern.h"
#include "arch.h"

extern uint64_t boot_pdpt[512];

void *vm_map_mmio(uint64_t phys, uint64_t len, int uncached)
{
    if (!len) return 0;
    uint64_t end = phys + len;
    if (end <= 0x100000000ULL) return (void *)(uintptr_t)phys;      /* already there */
    if (end > 512ULL << 30) return 0;                                /* one PDPT = 512 GiB */
    for (uint64_t a = phys & ~0x1FFFFFULL; a < end; a += 0x200000) {
        int pdpi = (int)(a >> 30), pdi = (int)((a >> 21) & 511);
        if (!(boot_pdpt[pdpi] & 1)) {
            uint64_t pg = pmm_alloc_page();
            if (!pg) return 0;
            memset((void *)(uintptr_t)pg, 0, PAGE_SIZE);
            boot_pdpt[pdpi] = pg | 3;
        }
        uint64_t *pd = (uint64_t *)(uintptr_t)(boot_pdpt[pdpi] & ~0xFFFULL);
        pd[pdi] = a | 0x83 | (uncached ? 0x18 : 0);                  /* P | RW | PS (| PWT | PCD) */
        __asm__ volatile("invlpg (%0)" :: "r"(a) : "memory");
    }
    return (void *)(uintptr_t)phys;
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

/* The framebuffer, write-combining. Runs before the page allocator, so page directories for
 * a framebuffer above 4 GiB come from here (2 GiB of 2 MiB pages is far more than any mode). */
static uint64_t fb_pd[2][512] __attribute__((aligned(4096)));
void *vm_map_fb(uint64_t phys, uint64_t len)
{
    uint64_t end = phys + len, attr = pat_ok ? 0x08 : 0x18;          /* PWT = WC, else UC */
    if (!len || end > 512ULL << 30) return 0;
    int used = 0;
    for (uint64_t a = phys & ~0x1FFFFFULL; a < end; a += 0x200000) {
        int pdpi = (int)(a >> 30), pdi = (int)((a >> 21) & 511);
        if (!(boot_pdpt[pdpi] & 1)) {
            if (used == 2) return 0;
            boot_pdpt[pdpi] = (uint64_t)(uintptr_t)fb_pd[used++] | 3;
        }
        uint64_t *pd = (uint64_t *)(uintptr_t)(boot_pdpt[pdpi] & ~0xFFFULL);
        pd[pdi] = a | 0x83 | attr;
        __asm__ volatile("invlpg (%0)" :: "r"(a) : "memory");
    }
    return (void *)(uintptr_t)phys;
}
