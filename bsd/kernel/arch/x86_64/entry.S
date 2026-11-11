/* Kernel entry and exit: interrupts, exceptions and the `syscall` instruction.
 *
 * User programs run on their own page tables, which map nothing of the kernel except this
 * section (code, IDT, GDT, TSS and a small entry stack), supervisor-only. That's what lets
 * ordinary Linux programs load at 0x400000, where the kernel's identity map of physical
 * memory would otherwise be. On entry from user mode we switch to the kernel's page tables
 * and move the trap frame onto the thread's own kernel stack; on the way back we do the
 * reverse. Traps taken in kernel mode stay on the current stack and page tables. */

        .section .entry.text, "ax"

.macro ISR_NOERR n
isr\n:
        pushq $0
        pushq $\n
        jmp trap_entry
.endm

.macro ISR_ERR n
isr\n:
        pushq $\n
        jmp trap_entry
.endm

.irp n, 0,1,2,3,4,5,6,7,9,15,16,18,19,20,22,23,24,25,26,27,28,31
        ISR_NOERR \n
.endr
.irp n, 8,10,11,12,13,14,17,21,29,30
        ISR_ERR \n
.endr
.irp n, 32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47
        ISR_NOERR \n
.endr
        ISR_NOERR 128

.macro PUSH_REGS
        pushq %rax
        pushq %rcx
        pushq %rdx
        pushq %rbx
        pushq %rbp
        pushq %rsi
        pushq %rdi
        pushq %r8
        pushq %r9
        pushq %r10
        pushq %r11
        pushq %r12
        pushq %r13
        pushq %r14
        pushq %r15
.endm

.macro POP_REGS
        popq %r15
        popq %r14
        popq %r13
        popq %r12
        popq %r11
        popq %r10
        popq %r9
        popq %r8
        popq %rdi
        popq %rsi
        popq %rbp
        popq %rbx
        popq %rdx
        popq %rcx
        popq %rax
.endm

#define FRAME 176               /* 15 registers + vector + error + rip cs rflags rsp ss */

trap_entry:
        testb $3, 24(%rsp)                       /* CS of the interrupted code */
        jnz from_user
        PUSH_REGS                                /* kernel mode: same stack, same page tables */
        movq %rsp, %rdi
        cld
        call trap_dispatch
        POP_REGS
        addq $16, %rsp
        iretq

/* syscall: rcx = user rip, r11 = user rflags; interrupts are off (SFMASK) */
        .global syscall_entry
syscall_entry:
        movq %rsp, user_rsp_scratch(%rip)
        leaq entry_stack_top(%rip), %rsp
        pushq $0x1b                              /* user ss */
        pushq user_rsp_scratch(%rip)
        pushq %r11
        pushq $0x23                              /* user cs */
        pushq %rcx
        pushq $0
        pushq $0x100                             /* "vector" 0x100: system call */
from_user:
        PUSH_REGS                                /* still on the entry stack, user page tables */
        movq kernel_cr3(%rip), %rax
        movq %rax, %cr3
        movq cur_kstack_top(%rip), %rdx
        subq $FRAME, %rdx
        movq %rdx, %rdi
        movq %rsp, %rsi
        movq $FRAME/8, %rcx
        cld
        rep movsq
        movq %rdx, %rsp                          /* the thread's kernel stack */
        movq %rsp, %rdi
        call trap_dispatch
        /* fall through */

/* return to user mode from the trap frame at %rsp */
        .global user_return_here
user_return_here:
        cli
        leaq entry_stack_top(%rip), %rdx
        subq $FRAME, %rdx
        movq %rdx, %rdi
        movq %rsp, %rsi
        movq $FRAME/8, %rcx
        cld
        rep movsq
        movq %rdx, %rsp
        movq cur_user_cr3(%rip), %rax
        movq %rax, %cr3
        POP_REGS
        addq $16, %rsp
        iretq

/* void user_return(struct trapframe *tf): enter user mode for the first time */
        .global user_return
user_return:
        movq %rdi, %rsp
        jmp user_return_here

        .section .entry.rodata, "a"
        .align 8
        .global isr_table
isr_table:
.irp n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47
        .quad isr\n
.endr
        .global isr128_addr
isr128_addr:
        .quad isr128

        .section .entry.data, "aw"
        .align 16
        .global kernel_cr3, cur_user_cr3, cur_kstack_top
kernel_cr3:       .quad 0
cur_user_cr3:     .quad 0
cur_kstack_top:   .quad 0
user_rsp_scratch: .quad 0
        .align 16
        .global entry_stack_top
entry_stack:      .fill 8192, 1, 0
entry_stack_top:

        .section .note.GNU-stack,"",@progbits
