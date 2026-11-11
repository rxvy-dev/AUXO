/*
 * AuxoBSD boot: Multiboot2 header, 32-bit entry, long-mode switch.
 * GRUB enters _start in 32-bit protected mode with EAX=0x36d76289, EBX=&mbi.
 */
        .set MB2_MAGIC, 0xe85250d6

        .section .multiboot, "a"
        .align 8
mb2_start:
        .long MB2_MAGIC
        .long 0                                  /* arch: i386 */
        .long mb2_end - mb2_start
        .long 0x100000000 - (MB2_MAGIC + 0 + (mb2_end - mb2_start))
        .align 8
        .short 0                                 /* end tag */
        .short 0
        .long 8
mb2_end:

        .section .boot, "ax"
        .code32
        .global _start
_start:
        cli
        cmpl $0x36d76289, %eax
        jne  no_multiboot
        movl %ebx, mb_info
        movl $boot_stack_top, %esp
        cld

        /* Long mode needs a 64-bit CPU. A VM set to a 32-bit guest type (VirtualBox's
         * "Other/Unknown") has none, and switching anyway triple-faults ("critical error"
         * in VirtualBox). Say so on screen instead. */
        movl $0x80000000, %eax
        cpuid
        cmpl $0x80000001, %eax
        jb   no_long_mode
        movl $0x80000001, %eax
        cpuid
        testl $(1 << 29), %edx                   /* EDX.LM */
        jz   no_long_mode

        /* PML4[0] -> PDPT */
        movl $pdpt, %eax
        orl  $3, %eax
        movl %eax, pml4

        /* PDPT[0..3] -> PD0..PD3 (4 GiB identity map) */
        xorl %ecx, %ecx
1:      movl %ecx, %eax
        shll $12, %eax
        addl $pd, %eax
        orl  $3, %eax
        movl %eax, pdpt(,%ecx,8)
        incl %ecx
        cmpl $4, %ecx
        jne  1b

        /* 2048 x 2 MiB pages: present | writable | huge */
        xorl %ecx, %ecx
2:      movl %ecx, %eax
        shll $21, %eax
        orl  $0x83, %eax
        cmpl $1536, %ecx                         /* >= 3 GiB: PCI MMIO window, map uncached */
        jb   4f
        orl  $0x18, %eax                         /* PCD | PWT */
4:
        movl %eax, pd(,%ecx,8)
        movl $0, pd+4(,%ecx,8)
        incl %ecx
        cmpl $2048, %ecx
        jne  2b

        movl $pml4, %eax
        movl %eax, %cr3
        movl %cr4, %eax
        orl  $0x20, %eax                         /* CR4.PAE */
        movl %eax, %cr4
        movl $0xC0000080, %ecx                   /* IA32_EFER */
        rdmsr
        orl  $0x100, %eax                        /* LME */
        wrmsr
        movl %cr0, %eax
        orl  $0x80000001, %eax                   /* PG | PE */
        movl %eax, %cr0

        lgdt boot_gdtr
        ljmp $0x08, $long_mode_entry

no_long_mode:
        movl $0xb8000, %edi                      /* clear the screen */
        movl $0x0720, %eax
        movl $2000, %ecx
        rep stosw
        movl $lm_msg, %esi
        movl $0xb8000, %edi
3:      lodsb
        testb %al, %al
        jz   4f
        cmpb $10, %al                            /* newline: next 80-column row */
        jne  5f
        movl %edi, %eax
        subl $0xb8000, %eax
        xorl %edx, %edx
        movl $160, %ecx
        divl %ecx
        incl %eax
        imull $160, %eax
        leal 0xb8000(%eax), %edi
        jmp  3b
5:      movb $0x0f, %ah
        stosw
        jmp  3b
4:      cli
        hlt
        jmp  4b

no_multiboot:
        movl $0x4f524f45, 0xb8000                /* "ER" */
        movl $0x4f3a4f52, 0xb8004                /* "R:" */
1:      hlt
        jmp  1b

        .code64
long_mode_entry:
        movw $0x10, %ax
        movw %ax, %ds
        movw %ax, %es
        movw %ax, %ss
        xorl %eax, %eax
        movw %ax, %fs
        movw %ax, %gs
        movq $boot_stack_top, %rsp
        xorl %ebp, %ebp
        movl mb_info(%rip), %edi
        call kmain
2:      cli
        hlt
        jmp  2b

        .section .rodata
lm_msg:
        .ascii "AuxoBSD needs a 64-bit (x86_64) CPU, and this machine doesn't report one.\n\n"
        .ascii "VirtualBox: power off, open Settings > General > Basic and set\n"
        .ascii "  Type: Other   Version: Other/Unknown (64-bit)   (or FreeBSD (64-bit))\n"
        .ascii "and make sure System > Processor has VT-x/AMD-V available.\n\n"
        .ascii "QEMU: use qemu-system-x86_64 without -cpu qemu32.\0"
        .align 16
boot_gdt:
        .quad 0
        .quad 0x00AF9A000000FFFF                 /* 0x08 64-bit code */
        .quad 0x00CF92000000FFFF                 /* 0x10 data */
boot_gdtr:
        .short . - boot_gdt - 1
        .long  boot_gdt

        .section .bss, "aw", @nobits        /* I like /* */ /* */
        .align 4096
pml4:   .skip 4096
pdpt:   .skip 4096
pd:     .skip 4096 * 4
        .align 16
boot_stack:
        .skip 32768
        .global boot_stack_top
boot_stack_top:
mb_info: .skip 8

        .section .note.GNU-stack,"",@progbits
