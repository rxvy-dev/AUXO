/* C-side trap dispatcher: CPU exceptions, IRQs, and the int 0x80 syscall gate. */
#include "kern.h"
#include "arch.h"
#include "rs.h"

static volatile uint64_t ticks;
uint64_t kernel_ticks(void) { return ticks; }

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

void trap_dispatch(struct trapframe *tf)
{
    if (tf->vector < 32) {
        exception(tf);
    } else if (tf->vector < 48) {
        int irq = (int)tf->vector - 32;
        if (irq == 1) input_kbd_irq();          /* read the device before EOI so it can raise again */
        else if (irq == 4) input_serial_irq();
        pic_eoi(irq);                 /* EOI first: the scheduler may not return here */
        if (irq == 0) { ticks++; rs_tick(); }
    } else if (tf->vector == 0x80) {
        /* BSD-style: number in rax, args in rdi/rsi/rdx, result in rax (<0 = -errno) */
        tf->rax = (uint64_t)rs_syscall(tf->rax, tf->rdi, tf->rsi, tf->rdx);
    }
}
