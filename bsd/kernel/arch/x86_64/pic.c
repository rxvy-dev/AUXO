/* 8259 PIC remap (IRQ0-15 -> vectors 32-47) and 8254 PIT. */
#include "kern.h"
#include "arch.h"

#define PIC1 0x20
#define PIC2 0xA0

void pic_init(void)
{
    outb(PIC1, 0x11); io_wait();  outb(PIC2, 0x11); io_wait();
    outb(PIC1 + 1, 32); io_wait(); outb(PIC2 + 1, 40); io_wait();
    outb(PIC1 + 1, 4);  io_wait(); outb(PIC2 + 1, 2);  io_wait();
    outb(PIC1 + 1, 1);  io_wait(); outb(PIC2 + 1, 1);  io_wait();
    outb(PIC1 + 1, 0xFE);            /* unmask IRQ0 (timer) only */
    outb(PIC2 + 1, 0xFF);
}

void pic_eoi(int irq)
{
    if (irq >= 8) outb(PIC2, 0x20);
    outb(PIC1, 0x20);
}

void pit_init(uint32_t hz)
{
    uint32_t div = 1193182 / hz;
    outb(0x43, 0x36);
    outb(0x40, div & 0xFF);
    outb(0x40, (div >> 8) & 0xFF);
}
