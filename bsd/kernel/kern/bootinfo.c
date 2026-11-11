/* Multiboot2 information: the command line (Auxo settings) and the loader's name. */
#include "kern.h"

#define ALIGN8(x) (((x) + 7) & ~7u)

static char cmdline[256];
static char loader[64];
static struct fbinfo fb;

void bootinfo_init(uint32_t mbi_addr)
{
    const uint8_t *mbi = (const uint8_t *)(uintptr_t)mbi_addr;
    uint32_t total = *(const uint32_t *)mbi;

    for (const uint8_t *p = mbi + 8; p < mbi + total; ) {
        uint32_t type = ((const uint32_t *)p)[0], size = ((const uint32_t *)p)[1];
        if (type == 0 || size < 8) break;
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
