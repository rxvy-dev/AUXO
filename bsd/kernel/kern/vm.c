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
