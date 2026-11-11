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

/* in the entry section: the CPU reads these while a user program's page tables are loaded */
static uint64_t   gdt[7] __attribute__((section(".entry.data"), aligned(16)));
static struct tss tss __attribute__((section(".entry.data"), aligned(16)));
extern char entry_stack_top[];
extern char syscall_entry[];
extern uint64_t kernel_cr3;

static void wrmsr(uint32_t m, uint64_t v) { __asm__ volatile("wrmsr" :: "c"(m), "a"((uint32_t)v), "d"((uint32_t)(v >> 32))); }
static uint64_t rdmsr(uint32_t m) { uint32_t lo, hi; __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(m)); return (uint64_t)hi << 32 | lo; }

void gdt_init(void)
{
    gdt[0] = 0;
    gdt[1] = 0x00AF9A000000FFFFULL;   /* 0x08 kernel code */
    gdt[2] = 0x00CF92000000FFFFULL;   /* 0x10 kernel data */
    gdt[3] = 0x00CFF2000000FFFFULL;   /* 0x18 user data   */
    gdt[4] = 0x00AFFA000000FFFFULL;   /* 0x20 user code   */

    memset(&tss, 0, sizeof tss);
    tss.rsp0  = (uint64_t)(uintptr_t)entry_stack_top;      /* traps from user mode land here first */
    tss.iomap = sizeof tss;

    uint64_t base = (uint64_t)(uintptr_t)&tss, limit = sizeof tss - 1;
    gdt[5] = (limit & 0xFFFF) | ((base & 0xFFFFFF) << 16) | (0x89ULL << 40) |
             (((limit >> 16) & 0xF) << 48) | (((base >> 24) & 0xFF) << 56);
    gdt[6] = base >> 32;              /* 0x28 TSS (16 bytes) */

    struct gdtr g = { sizeof gdt - 1, (uint64_t)(uintptr_t)gdt };
    gdt_flush(&g);

    /* the `syscall` instruction: kernel CS 0x08; sysret base 0x10 (user SS 0x1b, CS 0x23) */
    wrmsr(0xC0000080, rdmsr(0xC0000080) | 1);                  /* EFER.SCE */
    wrmsr(0xC0000081, (0x10ULL << 48) | (0x08ULL << 32));       /* STAR */
    wrmsr(0xC0000082, (uint64_t)(uintptr_t)syscall_entry);      /* LSTAR */
    wrmsr(0xC0000084, 0x200 | 0x400 | 0x100);                   /* SFMASK: IF, DF, TF off */
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    kernel_cr3 = cr3;
}

/* the scheduler tells the entry code which page tables and kernel stack the next thread uses */
extern uint64_t cur_user_cr3, cur_kstack_top;
void arch_switch_to(uint64_t ucr3, uint64_t kstack_top)
{
    cur_user_cr3 = ucr3 ? ucr3 : kernel_cr3;
    cur_kstack_top = kstack_top;
}
