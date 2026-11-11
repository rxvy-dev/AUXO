/* reboot / poweroff without ACPI: the 8042 reset line, then the emulators' shutdown ports. */
#include "kern.h"
#include "arch.h"

void machine_reboot(void)
{
    cli();
    for (int i = 0; i < 100000 && (inb(0x64) & 2); i++)
        ;
    outb(0x64, 0xFE);                         /* pulse the CPU reset line */
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) none = { 0, 0 };
    __asm__ volatile("lidt %0; int3" :: "m"(none));   /* no IDT: triple fault resets */
    for (;;) hlt();
}

void machine_poweroff(void)
{
    cli();
    outw(0x604, 0x2000);                      /* QEMU (q35 / i440fx with ACPI PM at 0x600) */
    outw(0xB004, 0x2000);                     /* Bochs, older QEMU */
    outw(0x4004, 0x3400);                     /* VirtualBox */
    outb(0xF4, 0x00);                         /* QEMU isa-debug-exit, if present */
    kprintf("It's now safe to turn off your computer.\n");
    for (;;) hlt();
}
