/* The linear framebuffer GRUB set up from our Multiboot2 framebuffer tag. The desktop draws into
 * a 32-bit XRGB back buffer; fb_present copies rectangles of it to the screen in whatever RGB
 * layout the mode has (32 or 24 bits per pixel). */
#include "kern.h"
#include "arch.h"

static const struct fbinfo *fb;
static uint8_t *mem;

int fb_init(void)
{
    fb = boot_framebuffer();
    if (!fb || fb->type != 1 || (fb->bpp != 32 && fb->bpp != 24 && fb->bpp != 16 && fb->bpp != 15) ||
        fb->width < 640 || fb->height < 480)
        return 0;
    /* NVIDIA and AMD cards with Resizable BAR / above-4G decoding put the framebuffer far
     * above 4 GiB; map it there, write-combining either way */
    pat_init();
    mem = vm_map_fb(fb->addr, (uint64_t)fb->pitch * fb->height);
    if (!mem) { fb = 0; return 0; }
    return 1;
}

uint64_t fb_phys(void) { return fb ? fb->addr : 0; }
int fb_width(void) { return fb ? (int)fb->width : 0; }
int fb_height(void) { return fb ? (int)fb->height : 0; }

/* src is a whole-screen buffer (same coordinates as the screen) when sx/sy = x/y, or a small
 * picture of its own when sx = sy = 0 (fb_blit) */
static void blit(const uint32_t *src, int stride, int sx, int sy, int x, int y, int w, int h)
{
    if (!mem) return;
    if (x < 0) { w += x; sx -= x; x = 0; }
    if (y < 0) { h += y; sy -= y; y = 0; }
    if (x + w > (int)fb->width) w = (int)fb->width - x;
    if (y + h > (int)fb->height) h = (int)fb->height - y;
    if (w <= 0 || h <= 0) return;
    int std = fb->bpp == 32 && fb->rpos == 16 && fb->gpos == 8 && fb->bpos == 0;
    for (int yy = y; yy < y + h; yy++) {
        const uint32_t *s = src + (size_t)(sy + yy - y) * (size_t)stride + sx;
        uint8_t *d = mem + (size_t)yy * fb->pitch + (size_t)x * (fb->bpp / 8);
        if (std) { memcpy(d, s, (size_t)w * 4); continue; }
        for (int i = 0; i < w; i++) {
            uint32_t p = s[i];
            uint32_t r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
            uint32_t v = (r >> (8 - fb->rsize)) << fb->rpos | (g >> (8 - fb->gsize)) << fb->gpos |
                         (b >> (8 - fb->bsize)) << fb->bpos;
            if (fb->bpp == 32) ((uint32_t *)d)[i] = v;
            else if (fb->bpp <= 16) ((uint16_t *)d)[i] = (uint16_t)v;
            else { d[i * 3] = (uint8_t)v; d[i * 3 + 1] = (uint8_t)(v >> 8); d[i * 3 + 2] = (uint8_t)(v >> 16); }
        }
    }
}

void fb_present(const uint32_t *src, int stride, int x, int y, int w, int h) { blit(src, stride, x, y, x, y, w, h); }
void fb_blit(const uint32_t *px, int stride, int x, int y, int w, int h) { blit(px, stride, 0, 0, x, y, w, h); }

/* the text console: move the whole picture up by `rows` pixel rows and clear what's left */
void fb_scroll(int rows, uint32_t bg)
{
    if (!mem || rows <= 0) return;
    int h = (int)fb->height;
    if (rows > h) rows = h;
    size_t line = (size_t)fb->width * ((fb->bpp + 7) / 8);
    for (int y = 0; y + rows < h; y++) memcpy(mem + (size_t)y * fb->pitch, mem + (size_t)(y + rows) * fb->pitch, line);
    static uint32_t blank[2048];
    int w = (int)fb->width < 2048 ? (int)fb->width : 2048;
    for (int i = 0; i < w; i++) blank[i] = bg;
    for (int y = h - rows; y < h; y++) fb_blit(blank, 0, 0, y, w, 1);
}
