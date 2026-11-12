#ifndef AUXO_ACPI_H
#define AUXO_ACPI_H
#include <stddef.h>
void        acpi_set_rsdp(const void *p, size_t len);   /* from the Multiboot2 ACPI tag */
void        acpi_init(void);                            /* after the page allocator */
const char *acpi_summary(void);
void        acpi_poweroff(void);                        /* returns only if it didn't work */
void        acpi_reboot(void);
#endif
