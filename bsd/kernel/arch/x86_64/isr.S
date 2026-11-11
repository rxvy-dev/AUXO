/* Interrupt/exception entry stubs and common trap path. */
        .text

.macro ISR_NOERR n
isr\n:
        pushq $0
        pushq $\n
        jmp trap_common
.endm

.macro ISR_ERR n
isr\n:
        pushq $\n
        jmp trap_common
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
        ISR_NOERR 128                            /* int 0x80: BSD syscall gate */

trap_common:
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
        movq %rsp, %rdi
        cld
        call trap_dispatch
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
        addq $16, %rsp                           /* vector + error code */
        iretq

        .section .rodata
        .align 8
        .global isr_table
isr_table:
.irp n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47
        .quad isr\n
.endr
        .global isr128_addr
isr128_addr:
        .quad isr128

        .section .note.GNU-stack,"",@progbits
