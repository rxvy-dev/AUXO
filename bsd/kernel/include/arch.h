#ifndef AUXO_ARCH_H
#define AUXO_ARCH_H
#include <stdint.h>

/* Layout must match the push order in arch/x86_64/isr.S */
struct trapframe {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rbx, rdx, rcx, rax;
    uint64_t vector, err;
    uint64_t rip, cs, rflags, rsp, ss;
};

static inline void outb(uint16_t p, uint8_t v) { __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"(p)); }
static inline uint8_t inb(uint16_t p) { uint8_t v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline void outw(uint16_t p, uint16_t v) { __asm__ volatile("outw %0, %1" :: "a"(v), "Nd"(p)); }
static inline void io_wait(void) { outb(0x80, 0); }
static inline void cpuid(uint32_t leaf, uint32_t r[4])
{
    __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(0));
}
static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
static inline void cli(void) { __asm__ volatile("cli" ::: "memory"); }
static inline void sti(void) { __asm__ volatile("sti" ::: "memory"); }
static inline void hlt(void) { __asm__ volatile("hlt"); }
static inline uint64_t read_cr2(void) { uint64_t v; __asm__ volatile("mov %%cr2, %0" : "=r"(v)); return v; }

void gdt_init(void);
void idt_init(void);
void pic_init(void);
void pic_eoi(int irq);
void pit_init(uint32_t hz);
void trap_dispatch(struct trapframe *tf);
uint64_t kernel_ticks(void);

/* assembly */
void gdt_flush(const void *gdtr);
void ctx_switch(uint64_t *old_rsp, uint64_t new_rsp);
void thread_trampoline(void);
extern char boot_stack_top[];

#endif
