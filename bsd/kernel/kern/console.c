/* Dual console: COM1 serial (115200 8N1) + VGA text mode 80x25.
 *
 * Output is written once, with ANSI escapes, and goes to both:
 *  - serial gets the bytes as they are (truecolour escapes and UTF-8 for a real terminal),
 *    or with the escapes stripped when colour is off;
 *  - VGA gets them through a small ANSI/UTF-8 decoder. Colours are matched to a 16-entry
 *    palette loaded into the VGA DAC, and slots 1-6 hold the Auxo accent gradient, so
 *    `auxo-tweak accent rose` recolours everything already on screen. */
#include "kern.h"
#include "arch.h"

#define COM1 0x3F8
#define VGA_W 80
#define VGA_H 25

/* palette slots */
enum { C_BG = 0, C_ACC0 = 1, C_ACC5 = 6, C_FG = 7, C_DIM = 8, C_RED = 9, C_GREEN = 10,
       C_YELLOW = 11, C_BLUE = 12, C_MAGENTA = 13, C_CYAN = 14, C_WHITE = 15 };

static uint32_t pal[16] = {
    0x0d0d14,                                               /* background (Auxo dark) */
    0xa78bfa, 0x8b9cf4, 0x6faeee, 0x53bfe8, 0x37d0e2, 0x22d3ee,   /* accent gradient */
    0xd4d4d8,                                               /* text */
    0x6b6b78,                                               /* dim */
    0xf87171, 0x4ade80, 0xfbbf24, 0x60a5fa, 0xc084fc, 0x22d3ee,
    0xffffff,
};

/* The text grid: VGA text memory at boot, or a RAM grid the Chrono Terminal window draws
 * (console_attach_grid). Cells use the VGA layout: low byte CP437 glyph, high byte colours. */
static volatile uint16_t *vga = (volatile uint16_t *)0xB8000;
static int grid_w = VGA_W, grid_h = VGA_H, textmode = 1;
static volatile uint32_t grid_gen;          /* bumped on every change, so the GUI knows to redraw */
static int cx, cy;
static uint8_t fg = C_FG;
static int serial_color = 1;

/* ── serial ─────────────────────────────────────────────────────────── */
static int serial_present;

/* VirtualBox has no serial port unless you add one: then every read returns 0xFF and
 * anything waiting on the UART would wait forever. Check the scratch register first. */
static void serial_init(void)
{
    outb(COM1 + 7, 0xAE);
    serial_present = inb(COM1 + 7) == 0xAE && inb(COM1 + 5) != 0xFF;
    if (!serial_present) return;
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x01);        /* divisor 1 = 115200 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);        /* 8N1 */
    outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x0B);        /* DTR | RTS | OUT2 (OUT2 gates the IRQ line) */
}

int console_serial_present(void) { return serial_present; }

static void serial_raw(char c)
{
    if (!serial_present) return;
    for (int spin = 0; spin < 100000 && !(inb(COM1 + 5) & 0x20); spin++)
        ;
    outb(COM1, (uint8_t)c);
}

static void serial_putc(char c)
{
    static int esc;              /* 0 text, 1 after ESC, 2 inside CSI */
    if (!serial_color) {
        if (esc == 1) { esc = (c == '[') ? 2 : 0; return; }
        if (esc == 2) { if (c >= 0x40 && c <= 0x7e) esc = 0; return; }
        if (c == 0x1b) { esc = 1; return; }
    }
    if (c == '\n') serial_raw('\r');
    serial_raw(c);
}

void console_serial_color(int on) { serial_color = on; }

/* ── VGA palette ────────────────────────────────────────────────────── */
static void vga_load_palette(void)
{
    inb(0x3DA);                                  /* reset the attribute controller flip-flop */
    for (int i = 0; i < 16; i++) { outb(0x3C0, (uint8_t)i); outb(0x3C0, (uint8_t)i); }  /* identity */
    outb(0x3C0, 0x10); outb(0x3C0, 0x04);        /* mode control: text, no blink (bright bg) */
    outb(0x3C0, 0x20);                           /* palette access done, display on */
    for (int i = 0; i < 16; i++) {
        outb(0x3C8, (uint8_t)i);
        outb(0x3C9, (uint8_t)((pal[i] >> 18) & 0x3F));
        outb(0x3C9, (uint8_t)((pal[i] >> 10) & 0x3F));
        outb(0x3C9, (uint8_t)((pal[i] >> 2) & 0x3F));
    }
}

static uint32_t lerp(uint32_t a, uint32_t b, int num, int den)
{
    uint32_t out = 0;
    for (int sh = 16; sh >= 0; sh -= 8) {
        int x = (int)((a >> sh) & 0xFF), y = (int)((b >> sh) & 0xFF);
        out |= (uint32_t)(x + (y - x) * num / den) << sh;
    }
    return out;
}

void console_set_accent(uint32_t c1, uint32_t c2)
{
    for (int i = 0; i <= C_ACC5 - C_ACC0; i++) pal[C_ACC0 + i] = lerp(c1, c2, i, C_ACC5 - C_ACC0);
    vga_load_palette();
    grid_gen++;                                   /* Chrono redraws the Terminal and its accents */
}

static uint8_t nearest(uint32_t rgb)
{
    int r = (int)(rgb >> 16) & 0xFF, g = (int)(rgb >> 8) & 0xFF, b = (int)rgb & 0xFF;
    uint8_t best = C_FG;
    long bestd = -1;
    for (int i = 1; i < 16; i++) {               /* never pick the background */
        int dr = r - (int)((pal[i] >> 16) & 0xFF), dg = g - (int)((pal[i] >> 8) & 0xFF),
            db = b - (int)(pal[i] & 0xFF);
        long d = 2L * dr * dr + 4L * dg * dg + 3L * db * db;
        if (bestd < 0 || d < bestd) { bestd = d; best = (uint8_t)i; }
    }
    if (bestd > 0 && (r + g + b) < 90) return C_DIM;   /* very dark text: keep it readable */
    return best;
}

static uint32_t xterm256(int n)
{
    static const uint32_t base[16] = { 0x000000, 0xcd0000, 0x00cd00, 0xcdcd00, 0x0000ee, 0xcd00cd,
        0x00cdcd, 0xe5e5e5, 0x7f7f7f, 0xff0000, 0x00ff00, 0xffff00, 0x5c5cff, 0xff00ff, 0x00ffff, 0xffffff };
    if (n < 16) return base[n];
    if (n >= 232) { uint32_t v = (uint32_t)(8 + (n - 232) * 10); return v << 16 | v << 8 | v; }
    n -= 16;
    static const uint8_t lv[6] = { 0, 95, 135, 175, 215, 255 };
    return (uint32_t)lv[n / 36] << 16 | (uint32_t)lv[(n / 6) % 6] << 8 | lv[n % 6];
}

/* ── VGA text ───────────────────────────────────────────────────────── */
static inline uint16_t cell(uint8_t ch) { return (uint16_t)((C_BG << 12) | (fg << 8) | ch); }

static void vga_cursor(void)
{
    grid_gen++;
    if (!textmode) return;
    uint16_t pos = (uint16_t)(cy * grid_w + cx);
    outb(0x3D4, 0x0F); outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E); outb(0x3D5, (uint8_t)(pos >> 8));
}

static void vga_clear(void)
{
    for (int i = 0; i < grid_w * grid_h; i++) vga[i] = (uint16_t)((C_BG << 12) | (C_FG << 8) | ' ');
    cx = cy = 0;
    vga_cursor();
}

static void vga_glyph(uint8_t c)
{
    if (c == '\n') { cx = 0; cy++; }
    else if (c == '\r') cx = 0;
    else if (c == '\t') cx = (cx + 8) & ~7;
    else if (c == '\b') { if (cx > 0) cx--; }
    else { vga[cy * grid_w + cx] = cell(c); cx++; }

    if (cx >= grid_w) { cx = 0; cy++; }
    if (cy >= grid_h) {
        for (int i = 0; i < grid_w * (grid_h - 1); i++) vga[i] = vga[i + grid_w];
        for (int i = 0; i < grid_w; i++) vga[(grid_h - 1) * grid_w + i] = (uint16_t)((C_BG << 12) | (C_FG << 8) | ' ');
        cy = grid_h - 1;
    }
}

/* Unicode → code page 437 for the glyphs the Auxo tools use */
static uint8_t cp437(uint32_t u)
{
    switch (u) {
    case 0x25CF: return 0x07;   /* ● */
    case 0x2022: return 0x07;   /* • */
    case 0x25CB: return 0x09;   /* ○ */
    case 0x2500: case 0x2501: return 0xC4;   /* ─ ━ */
    case 0x2588: return 0xDB;   /* █ */
    case 0x25B2: return 0x1E;   /* ▲ */
    case 0x276F: return 0xAF;   /* ❯ → » */
    case 0x2665: return 0x03;   /* ♥ */
    case 0x2726: return 0x04;   /* ✦ → ♦ */
    case 0x00B7: return 0xFA;   /* · */
    case 0x2713: return 0xFB;   /* ✓ → √ */
    case 0x2192: return 0x1A;   /* → */
    case 0x2014: case 0x2013: return '-';
    case 0x2026: return '.';
    default:     return u < 0x80 ? (uint8_t)u : '?';
    }
}

static void sgr(const int *p, int np)
{
    if (np == 0) { fg = C_FG; return; }
    for (int i = 0; i < np; i++) {
        int v = p[i];
        if (v == 0 || v == 39 || v == 22) fg = C_FG;
        else if (v == 1) { if (fg == C_FG) fg = C_WHITE; }
        else if (v == 2) fg = C_DIM;
        else if (v >= 30 && v <= 37) {
            static const uint8_t m[8] = { C_DIM, C_RED, C_GREEN, C_YELLOW, C_BLUE, C_MAGENTA, C_CYAN, C_FG };
            fg = m[v - 30];
        } else if (v >= 90 && v <= 97) {
            static const uint8_t m[8] = { C_DIM, C_RED, C_GREEN, C_YELLOW, C_BLUE, C_MAGENTA, C_CYAN, C_WHITE };
            fg = m[v - 90];
        } else if (v == 38 && i + 2 < np && p[i + 1] == 5) {
            fg = nearest(xterm256(p[i + 2])); i += 2;
        } else if (v == 38 && i + 4 < np && p[i + 1] == 2) {
            fg = nearest((uint32_t)(p[i + 2] & 0xFF) << 16 | (uint32_t)(p[i + 3] & 0xFF) << 8 | (uint32_t)(p[i + 4] & 0xFF));
            i += 4;
        }
    }
}

static void vga_putc(char ch)
{
    static int state;            /* 0 text, 1 ESC, 2 CSI */
    static int par[16], np, cur, have;
    static uint32_t u, need;     /* UTF-8 decoder */
    uint8_t c = (uint8_t)ch;

    if (state == 1) {
        if (c == '[') { state = 2; np = 0; cur = 0; have = 0; } else state = 0;
        return;
    }
    if (state == 2) {
        if (c >= '0' && c <= '9') { cur = cur * 10 + (c - '0'); have = 1; return; }
        if (c == ';') { if (np < 16) par[np++] = cur; cur = 0; have = 0; return; }
        if (have || np) { if (np < 16) par[np++] = cur; }
        state = 0;
        if (c == 'm') sgr(par, np);
        else if (c == 'J' && np && par[0] == 2) vga_clear();
        else if (c == 'H') { cx = cy = 0; }
        else if (c == 'K') { for (int x = cx; x < grid_w; x++) vga[cy * grid_w + x] = cell(' '); }
        return;
    }
    if (c == 0x1b) { state = 1; return; }

    if (need) {
        if ((c & 0xC0) == 0x80) { u = (u << 6) | (c & 0x3F); if (--need == 0) vga_glyph(cp437(u)); return; }
        need = 0;                                /* broken sequence: fall through */
    }
    if (c >= 0xF0) { u = c & 0x07; need = 3; return; }
    if (c >= 0xE0) { u = c & 0x0F; need = 2; return; }
    if (c >= 0xC0) { u = c & 0x1F; need = 1; return; }
    if (c == '\b') { vga_glyph('\b'); return; }
    vga_glyph(c);
}

/* ── the Chrono Terminal's grid ─────────────────────────────────────── */
void console_attach_grid(uint16_t *cells, int w, int h)
{
    /* keep what's on screen: copy the text console's rows into the new grid, bottom-aligned */
    int used = cx || cy ? cy + 1 : 0;            /* nothing written yet: copy nothing */
    int rows = used < h ? used : h, from = used - rows;
    for (int i = 0; i < w * h; i++) cells[i] = (uint16_t)((C_BG << 12) | (C_FG << 8) | ' ');
    for (int y = 0; y < rows; y++)
        for (int x = 0; x < w && x < grid_w; x++) cells[y * w + x] = vga[(from + y) * grid_w + x];
    vga = cells;
    grid_w = w; grid_h = h; textmode = 0;
    cy = rows - 1 < 0 ? 0 : rows - 1; cx = 0;
    grid_gen++;
}

void console_grid_info(int *w, int *h, int *cur_x, int *cur_y, uint32_t *gen)
{
    *w = grid_w; *h = grid_h; *cur_x = cx; *cur_y = cy; *gen = grid_gen;
}

const uint32_t *console_palette(void) { return pal; }

/* ── public ─────────────────────────────────────────────────────────── */
void console_init(void) { serial_init(); vga_load_palette(); vga_clear(); }

void console_clear(void)
{
    vga_clear();
    if (serial_color) { serial_raw(0x1b); serial_raw('['); serial_raw('2'); serial_raw('J');
                        serial_raw(0x1b); serial_raw('['); serial_raw('H'); }
}

void console_putc(char c)
{
    serial_putc(c);
    vga_putc(c);
    if (c == '\n' || (uint8_t)c < 0x80) vga_cursor();
}

void console_write(const char *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) console_putc(buf[i]);
}
