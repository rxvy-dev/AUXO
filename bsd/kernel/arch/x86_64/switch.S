/* Low-level helpers: GDT reload and kernel-thread context switch. */
        .text

/* void gdt_flush(const struct gdtr *p) */
        .global gdt_flush
gdt_flush:
        lgdt (%rdi)
        movw $0x10, %ax
        movw %ax, %ds
        movw %ax, %es
        movw %ax, %ss
        xorl %eax, %eax
        movw %ax, %fs
        movw %ax, %gs
        pushq $0x08
        leaq 1f(%rip), %rax
        pushq %rax
        lretq
1:      movw $0x28, %ax
        ltr  %ax
        ret

/* void ctx_switch(uint64_t *old_rsp, uint64_t new_rsp) */
        .global ctx_switch
ctx_switch:
        pushq %rbp
        pushq %rbx
        pushq %r12
        pushq %r13
        pushq %r14
        pushq %r15
        movq %rsp, (%rdi)
        movq %rsi, %rsp
        popq %r15
        popq %r14
        popq %r13
        popq %r12
        popq %rbx
        popq %rbp
        ret

/* First code a new kernel thread runs. r12 = entry, r13 = argument. */
        .global thread_trampoline
thread_trampoline:
        fninit                                   /* fresh x87/SSE state for the new thread */
        pushq $0x1F80
        ldmxcsr (%rsp)
        addq $8, %rsp
        sti
        movq %r13, %rdi
        call *%r12
        call rs_thread_exit
        ud2

        .section .note.GNU-stack,"",@progbits
