#include "kern.h"
#include "acpi.h"
#include "usb.h"
#include "arch.h"
#include "rs.h"
#include "auxo.h"
#include "chrono.h"
#include "pci.h"
#include "net.h"
#include "vfs.h"
#include "fs.h"
#include "disk.h"
#include "install.h"

#define HEAP_PAGES 4096   /* 16 MiB kernel heap, managed by Rust (thread stacks, fs nodes) */

/* Let the Chrono thread use SSE: CR0.EM off, CR0.MP on, CR4.OSFXSR + OSXMMEXCPT. The rest of the
 * kernel is built with -mgeneral-regs-only, so the XMM registers only ever hold Chrono's state. */
static void sse_enable(void)
{
    uint64_t cr0, cr4;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 = (cr0 & ~(1UL << 2)) | (1UL << 1);
    __asm__ volatile("mov %0, %%cr0" :: "r"(cr0));
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1UL << 9) | (1UL << 10);
    __asm__ volatile("mov %0, %%cr4" :: "r"(cr4));
    __asm__ volatile("fninit");
}

static void ok(const char *tag)
{
    c_rgb(auxo_c1()); kprintf("%-4s", tag); c_reset(); kprintf(": ");
}

void kmain(uint32_t mbi)
{
    bootinfo_init(mbi);
    auxo_conf_init(boot_cmdline());   /* accent / colour first, so the boot log is drawn in them */
    console_init();
    int gui = fb_init();              /* graphics mode: the console goes to Chrono's Terminal */
    if (gui) chrono_attach_console();

    c_rgb_bold(auxo_c1()); kprintf(AUXO_NAME " " AUXO_VERSION); c_reset();
    kprintf(" (x86_64) - C + Rust + Assembly · part of Auxo · " AUXO_URL "\n");
    ok("boot"); kprintf("long mode OK%s%s\n", boot_loader()[0] ? ", loaded by " : "", boot_loader());

    gdt_init();  ok("gdt");  kprintf("kernel/user segments + TSS loaded\n");
    idt_init();  ok("idt");  kprintf("48 vectors + int 0x80 syscall gate\n");
    pic_init();
    tsc_calibrate();

    pmm_init(mbi);
    ok("pmm");
    kprintf("%lu MiB usable, %lu MiB free\n",
            (unsigned long)(pmm_total_pages() * PAGE_SIZE >> 20),
            (unsigned long)(pmm_free_pages() * PAGE_SIZE >> 20));

    uint64_t heap = pmm_alloc_contig(HEAP_PAGES);
    if (!heap) panic("cannot allocate kernel heap");
    rs_init((uintptr_t)heap, HEAP_PAGES * PAGE_SIZE);
    ok("rust"); kprintf("heap + scheduler up (heap at %p)\n", (void *)heap);
    pit_init(AUXO_HZ);                /* now, not at the end: drivers' delays count PIT ticks, and a
                                         UEFI boot doesn't leave the PIT running the way a BIOS does */
    acpi_init();
    ok("acpi"); kprintf("%s\n", acpi_summary());

    input_init();
    int mouse = gui ? mouse_init() : 0;
    if (gui) {
        ok("fb");
        kprintf("%dx%d framebuffer, PS/2 mouse %s\n", fb_width(), fb_height(), mouse ? "found" : "not found");
    }
    ok("tty");  kprintf("%s%s, %s\n", gui ? "Chrono Terminal" : "VGA text 80x25",
                        console_serial_present() ? " + ttyS0 115200" : "", inb(0x64) == 0xFF ? "no PS/2 controller" : "PS/2 keyboard");
    ok("auxo"); kprintf("accent %s, hostname %s\n", auxo_accent_name(), auxo.hostname);

    if (gui) {
        sse_enable();
        if (chrono_start() < 0) panic("cannot start the Chrono desktop");
        ok("gui"); kprintf("Chrono desktop, %d themes (github.com/rxvy-dev/chrono)\n", CHRONO_THEMES);
    }
    fs_init();
    const uint8_t *img; size_t imglen;
    if (boot_module("rootfs", &img, &imglen) == 0) {
        int n = fs_mount_tar(img, imglen);
        ok("root"); kprintf("%d files from the root filesystem image (%lu MiB): Linux programs run\n", n, (unsigned long)(imglen >> 20));
        if (boot_module("overlay", &img, &imglen) == 0) fs_mount_tar(img, imglen);   /* pkg, on top */
    } else { ok("root"); kprintf("no root filesystem image: built-in commands only\n"); }
    vfs_init();
    pci_init();
    const struct pci_dev *gpu = pci_gpu();
    char gname[64] = "none";
    if (gpu) pci_describe(gpu, gname, sizeof gname);
    ok("pci"); kprintf("%d devices, display: %s%s\n", pci_count(), gname, gui ? " (firmware framebuffer)" : "");
    if (usb_init()) { ok("usb"); kprintf("%d xHCI controller%s: USB keyboards, mice and hubs\n", usb_controllers(), usb_controllers() > 1 ? "s" : ""); }
    disk_init();
    if (disk_count()) {
        ok("disk");
        for (int i = 0; i < disk_count(); i++) {
            struct disk *d = disk_get(i);
            kprintf("%s%s %s %lu GB", i ? ", " : "", d->name, d->bus, (unsigned long)(d->sectors * 512 / 1000000000));
        }
        kprintf("\n");
        if (store_init() == 0) { ok("home"); kprintf("/home/root is kept on %s (saved every 10 s)\n", store_where()); }
    }
    net_init();                       /* the net thread: Ethernet driver + DHCP, in the background */
    if (rs_spawn("auxsh", shell_main, 0) < 0) panic("cannot start the shell");
    sti();

    for (;;) {                       /* pid 0: the idle thread */
        rs_reap();
        hlt();
    }
}
