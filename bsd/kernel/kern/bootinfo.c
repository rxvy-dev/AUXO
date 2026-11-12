/* Multiboot2 information: the command line (Auxo settings) and the loader's name. */
#include "kern.h"
#include "acpi.h"

#define ALIGN8(x) (((x) + 7) & ~7u)

static char cmdline[256];
static char loader[64];
static struct fbinfo fb;
static struct { uint32_t start, end; char name[32]; } mods[8];
static int nmods;

void bootinfo_init(uint32_t mbi_addr)
{
    const uint8_t *mbi = (const uint8_t *)(uintptr_t)mbi_addr;
    uint32_t total = *(const uint32_t *)mbi;

    for (const uint8_t *p = mbi + 8; p < mbi + total; ) {
        uint32_t type = ((const uint32_t *)p)[0], size = ((const uint32_t *)p)[1];
        if (type == 0 || size < 8) break;
        if (type == 3 && nmods < 8) {                                         /* module */
            mods[nmods].start = *(const uint32_t *)(p + 8);
            mods[nmods].end = *(const uint32_t *)(p + 12);
            strlcpy(mods[nmods].name, (const char *)p + 16, sizeof mods[nmods].name);
            nmods++;
        }
        if ((type == 14 || type == 15) && size > 8) acpi_set_rsdp(p + 8, size - 8);   /* ACPI RSDP (v1/v2) */
        if (type == 1) strlcpy(cmdline, (const char *)p + 8, sizeof cmdline);   /* boot command line */
        if (type == 2) strlcpy(loader, (const char *)p + 8, sizeof loader);     /* boot loader name */
        if (type == 8 && size >= 31) {                                          /* framebuffer info */
            fb.addr = *(const uint64_t *)(p + 8);
            fb.pitch = *(const uint32_t *)(p + 16);
            fb.width = *(const uint32_t *)(p + 20);
            fb.height = *(const uint32_t *)(p + 24);
            fb.bpp = p[28];
            fb.type = p[29];
            if (fb.type == 1) {                                                 /* direct RGB: channel layout */
                fb.rpos = p[32]; fb.rsize = p[33]; fb.gpos = p[34]; fb.gsize = p[35]; fb.bpos = p[36]; fb.bsize = p[37];
            }
        }
        p += ALIGN8(size);
    }
}

const char *boot_cmdline(void) { return cmdline; }
const char *boot_loader(void) { return loader; }
const struct fbinfo *boot_framebuffer(void) { return fb.addr ? &fb : 0; }

int boot_module(const char *name, const uint8_t **data, size_t *len)
{
    for (int i = 0; i < nmods; i++)
        if (!strcmp(mods[i].name, name)) { *data = (const uint8_t *)(uintptr_t)mods[i].start; *len = mods[i].end - mods[i].start; return 0; }
    return -1;
}
