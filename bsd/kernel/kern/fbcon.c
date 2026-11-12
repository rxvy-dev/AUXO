/* Text on the framebuffer, without the desktop: what the console looks like when there's no
 * VGA text mode to fall back on (every UEFI PC, VirtualBox and Hyper-V in EFI mode), when the
 * `text` boot option asks for no desktop, and when the kernel panics under the desktop: the
 * compositor can't run then, so the console draws the message itself. Cells are VGA-style
 * (glyph byte, colour nibbles), drawn 8x16 with the Terminal's monospace font. */
#include "kern.h"
#include "gfx.h"
#include "fonts.h"

void fbcon_cell(int x, int y, uint16_t cell, const uint32_t *pal)
{
    static uint32_t px[8 * 16];
    struct canvas c;
    canvas_init(&c, px, 8, 16, 8);
    int ch = cell & 0xff, fg = (cell >> 8) & 15, bg = (cell >> 12) & 15;
    gfx_fill(&c, 0, 0, 8, 16, pal[bg] | 0xff000000);
    if (ch && ch != ' ') {
        static const char *const cp[256] = { [0x07] = "●", [0x09] = "○", [0xC4] = "─", [0xDB] = "█",
            [0x1E] = "▲", [0xAF] = "❯", [0x03] = "♥", [0x04] = "✦", [0xFA] = "·", [0xFB] = "✓", [0x1A] = "→" };
        char s[2] = { (char)(ch < 127 ? ch : '?'), 0 };
        gfx_text(&c, &font_mono, 0, 0, cp[ch] ? cp[ch] : s, pal[fg] | 0xff000000);
    }
    fb_blit(px, 8, x * 8, y * 16, 8, 16);
}
