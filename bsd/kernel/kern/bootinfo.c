/* Multiboot2 information: the command line (Auxo settings) and the loader's name. */
#include "kern.h"

#define ALIGN8(x) (((x) + 7) & ~7u)

static char cmdline[256];
static char loader[64];

void bootinfo_init(uint32_t mbi_addr)
{
    const uint8_t *mbi = (const uint8_t *)(uintptr_t)mbi_addr;
    uint32_t total = *(const uint32_t *)mbi;

    for (const uint8_t *p = mbi + 8; p < mbi + total; ) {
        uint32_t type = ((const uint32_t *)p)[0], size = ((const uint32_t *)p)[1];
        if (type == 0 || size < 8) break;
        if (type == 1) strlcpy(cmdline, (const char *)p + 8, sizeof cmdline);   /* boot command line */
        if (type == 2) strlcpy(loader, (const char *)p + 8, sizeof loader);     /* boot loader name */
        p += ALIGN8(size);
    }
}

const char *boot_cmdline(void) { return cmdline; }
const char *boot_loader(void) { return loader; }
