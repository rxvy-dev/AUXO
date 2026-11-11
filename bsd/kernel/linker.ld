/* AuxoBSD linker script: kernel is loaded at 1 MiB by a Multiboot2 loader. */
ENTRY(_start)
SECTIONS
{
    . = 1M;
    __kernel_start = .;

    .multiboot : { KEEP(*(.multiboot)) }
    /* the only part of the kernel user programs' page tables map (see arch/x86_64/entry.S) */
    .entry : ALIGN(4096) {
        __entry_start = .;
        *(.entry.text) *(.entry.rodata) *(.entry.data)
        . = ALIGN(4096);
        __entry_end = .;
    }
    .text : ALIGN(4096) { *(.boot) *(.text .text.*) }
    .rodata : ALIGN(4096) { *(.rodata .rodata.*) }
    .data : ALIGN(4096) { *(.data .data.*) }
    .bss : ALIGN(4096) { *(COMMON) *(.bss .bss.*) }

    . = ALIGN(4096);
    __kernel_end = .;

    /DISCARD/ : { *(.eh_frame*) *(.note*) *(.comment) }
}
