/* C-side trap dispatcher: CPU exceptions, IRQs, and the int 0x80 syscall gate. */
#include "kern.h"
#include "arch.h"
#include "rs.h"
#include "uproc.h"

static volatile uint64_t ticks;
uint64_t kernel_ticks(void) { return ticks; }

/* The TSC, calibrated against PIT channel 2 (no interrupts needed), for delays on the boot
 * path: kmain is the idle thread, which only runs when nothing else wants the CPU, so it can't
 * sleep on timer ticks once other threads (the desktop) are busy. */
static uint64_t tsc_per_ms;

void tsc_calibrate(void)
{
    uint8_t g = inb(0x61);
    outb(0x61, (uint8_t)((g & ~0x03) | 0x00));        /* gate off, speaker off */
    outb(0x43, 0xB0);                                 /* channel 2, lo/hi, mode 0 */
    outb(0x42, 11932 & 0xFF);                         /* 10 ms at 1.193182 MHz */
    outb(0x42, 11932 >> 8);
    outb(0x61, (uint8_t)((g & ~0x03) | 0x01));        /* gate on: count starts */
    uint64_t t0 = rdtsc();
    uint32_t i = 0;
    while (!(inb(0x61) & 0x20) && ++i < 50000000u) { }
    uint64_t t1 = rdtsc();
    outb(0x61, g);
    if (i < 50000000u && t1 > t0) tsc_per_ms = (t1 - t0) / 10;
    if (!tsc_per_ms) {                                /* no PIT channel 2: ask the CPU */
        uint32_t r[4];
        cpuid(0, r);
        if (r[0] >= 0x16) { cpuid(0x16, r); if (r[0]) tsc_per_ms = (uint64_t)(r[0] & 0xFFFF) * 1000; }
    }
    if (!tsc_per_ms) tsc_per_ms = 2000000;            /* assume 2 GHz rather than hang */
}

void kdelay_ms(uint32_t ms)
{
    uint64_t f;
    __asm__ volatile("pushfq; pop %0" : "=r"(f));
    if (rs_current_pid() != 0 && (f & 0x200)) {       /* a thread: sleep, let others run */
        rs_sleep((uint64_t)(ms * AUXO_HZ + 999) / 1000);
        return;
    }
    if (!tsc_per_ms) tsc_calibrate();
    uint64_t end = rdtsc() + tsc_per_ms * ms;
    while (rdtsc() < end) __asm__ volatile("pause");
}

static const char *const exc_name[32] = {
    "Divide Error", "Debug", "NMI", "Breakpoint", "Overflow", "Bound Range",
    "Invalid Opcode", "Device N/A", "Double Fault", "Coproc Overrun", "Invalid TSS",
    "Segment Not Present", "Stack Fault", "General Protection", "Page Fault",
    "Reserved", "x87 FP", "Alignment Check", "Machine Check", "SIMD FP",
    "Virtualization", "Control Protection", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Hypervisor Inj", "VMM Comm",
    "Security", "Reserved"
};

static void exception(struct trapframe *tf)
{
    kprintf("\n--- CPU exception %lu: %s (err=%lx) ---\n", tf->vector, exc_name[tf->vector], tf->err);
    kprintf("rip=%016lx cs=%lx rflags=%lx\n", tf->rip, tf->cs, tf->rflags);
    kprintf("rax=%016lx rbx=%016lx rcx=%016lx\n", tf->rax, tf->rbx, tf->rcx);
    kprintf("rdx=%016lx rsi=%016lx rdi=%016lx\n", tf->rdx, tf->rsi, tf->rdi);
    kprintf("rbp=%016lx rsp=%016lx\n", tf->rbp, tf->rsp);
    if (tf->vector == 14) kprintf("cr2=%016lx\n", read_cr2());
    panic("unhandled exception in kernel mode");
}

void uproc_irq_return(struct trapframe *tf);

void trap_dispatch(struct trapframe *tf)
{
    int user = (tf->cs & 3) != 0;
    if (tf->vector == 0x100) { syscall_dispatch(tf); return; }      /* `syscall` from a user program */
    if (tf->vector < 32) {
        if (user) { user_exception(tf); return; }
        exception(tf);
    } else if (tf->vector < 48) {
        int irq = (int)tf->vector - 32;
        if (irq == 1) input_kbd_irq();          /* read the device before EOI so it can raise again */
        else if (irq == 4) input_serial_irq();
        else if (irq == 12) mouse_irq();
        pic_eoi(irq);                 /* EOI first: the scheduler may not return here */
        if (irq == 0) { ticks++; rs_tick(); }
        if (user) uproc_irq_return(tf);
    } else if (tf->vector == 0x80) {
        /* BSD-style: number in rax, args in rdi/rsi/rdx, result in rax (<0 = -errno) */
        tf->rax = (uint64_t)rs_syscall(tf->rax, tf->rdi, tf->rsi, tf->rdx);
    }
}
