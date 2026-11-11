/* GDT + TSS. Order matches SYSRET expectations: kcode, kdata, udata, ucode. */
#include "kern.h"
#include "arch.h"

struct tss {
    uint32_t r0;
    uint64_t rsp0, rsp1, rsp2;
    uint64_t r1;
    uint64_t ist[7];
    uint64_t r2;
    uint16_t r3, iomap;
} __attribute__((packed));

struct gdtr { uint16_t limit; uint64_t base; } __attribute__((packed));

static uint64_t   gdt[7];
static struct tss tss;

void gdt_init(void)
{
    gdt[0] = 0;
    gdt[1] = 0x00AF9A000000FFFFULL;   /* 0x08 kernel code */
    gdt[2] = 0x00CF92000000FFFFULL;   /* 0x10 kernel data */
    gdt[3] = 0x00CFF2000000FFFFULL;   /* 0x18 user data   */
    gdt[4] = 0x00AFFA000000FFFFULL;   /* 0x20 user code   */

    memset(&tss, 0, sizeof tss);
    tss.rsp0  = (uint64_t)(uintptr_t)boot_stack_top;
    tss.iomap = sizeof tss;

    uint64_t base = (uint64_t)(uintptr_t)&tss, limit = sizeof tss - 1;
    gdt[5] = (limit & 0xFFFF) | ((base & 0xFFFFFF) << 16) | (0x89ULL << 40) |
             (((limit >> 16) & 0xF) << 48) | (((base >> 24) & 0xFF) << 56);
    gdt[6] = base >> 32;              /* 0x28 TSS (16 bytes) */

    struct gdtr g = { sizeof gdt - 1, (uint64_t)(uintptr_t)gdt };
    gdt_flush(&g);
}
