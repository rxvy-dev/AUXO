#include "kern.h"
#include "arch.h"

struct idt_entry {
    uint16_t off_lo, sel;
    uint8_t  ist, attr;
    uint16_t off_mid;
    uint32_t off_hi, zero;
} __attribute__((packed));

extern const uint64_t isr_table[48];
extern const uint64_t isr128_addr;

static struct idt_entry idt[256];

static void set_gate(int n, uint64_t h, uint8_t attr)
{
    idt[n].off_lo  = h & 0xFFFF;
    idt[n].sel     = 0x08;
    idt[n].ist     = 0;
    idt[n].attr    = attr;
    idt[n].off_mid = (h >> 16) & 0xFFFF;
    idt[n].off_hi  = (uint32_t)(h >> 32);
    idt[n].zero    = 0;
}

void idt_init(void)
{
    for (int i = 0; i < 48; i++) set_gate(i, isr_table[i], 0x8E);
    set_gate(0x80, isr128_addr, 0xEE);          /* DPL 3: callable from userland */

    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) p =
        { sizeof idt - 1, (uint64_t)(uintptr_t)idt };
    __asm__ volatile("lidt %0" :: "m"(p));
}
