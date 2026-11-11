#include "kern.h"
#include "arch.h"
#include "rs.h"
#include "auxo.h"

#define HEAP_PAGES 1024   /* 4 MiB kernel heap, managed by Rust */

static void ok(const char *tag)
{
    c_rgb(auxo_c1()); kprintf("%-4s", tag); c_reset(); kprintf(": ");
}

void kmain(uint32_t mbi)
{
    bootinfo_init(mbi);
    auxo_conf_init(boot_cmdline());   /* accent / colour first, so the boot log is drawn in them */
    console_init();

    c_rgb_bold(auxo_c1()); kprintf(AUXO_NAME " " AUXO_VERSION); c_reset();
    kprintf(" (x86_64) - C + Rust + Assembly · part of Auxo · " AUXO_URL "\n");
    ok("boot"); kprintf("long mode OK%s%s\n", boot_loader()[0] ? ", loaded by " : "", boot_loader());

    gdt_init();  ok("gdt");  kprintf("kernel/user segments + TSS loaded\n");
    idt_init();  ok("idt");  kprintf("48 vectors + int 0x80 syscall gate\n");
    pic_init();

    pmm_init(mbi);
    ok("pmm");
    kprintf("%lu MiB usable, %lu MiB free\n",
            (unsigned long)(pmm_total_pages() * PAGE_SIZE >> 20),
            (unsigned long)(pmm_free_pages() * PAGE_SIZE >> 20));

    uint64_t heap = pmm_alloc_contig(HEAP_PAGES);
    if (!heap) panic("cannot allocate kernel heap");
    rs_init((uintptr_t)heap, HEAP_PAGES * PAGE_SIZE);
    ok("rust"); kprintf("heap + scheduler up (heap at %p)\n", (void *)heap);

    input_init();
    ok("tty");  kprintf("VGA text 80x25%s, PS/2 keyboard\n", console_serial_present() ? " + ttyS0 115200" : "");
    ok("auxo"); kprintf("accent %s, hostname %s\n", auxo_accent_name(), auxo.hostname);

    if (rs_spawn("auxsh", shell_main, 0) < 0) panic("cannot start the shell");
    pit_init(AUXO_HZ);
    sti();

    for (;;) {                       /* pid 0: the idle thread */
        rs_reap();
        hlt();
    }
}
