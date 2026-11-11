/* Keyboard input: COM1 receive and a PS/2 keyboard (scancode set 1), both on IRQs,
 * feeding one byte queue. Keys without a byte (arrows, Home, End, Delete) are queued as
 * the VT100 sequences a serial terminal sends, so the shell only has to read one format. */
#include "kern.h"
#include "arch.h"

#define COM1 0x3F8
#define QSIZE 256

static volatile uint8_t q[QSIZE];
static volatile uint32_t qhead, qtail;
/* keys for the Chrono desktop while it's running (the shell gets them via input_push) */
static volatile uint8_t gq[QSIZE];
static volatile uint32_t ghead, gtail;
static volatile int gui;

static void push(uint8_t c)
{
    uint32_t n = (qhead + 1) % QSIZE;
    if (n != qtail) { q[qhead] = c; qhead = n; }
}

static void kpush(uint8_t c)               /* a key from the keyboard: shell or desktop */
{
    if (!gui) { if (c < 0x80) push(c); return; }
    uint32_t n = (ghead + 1) % QSIZE;
    if (n != gtail) { gq[ghead] = c; ghead = n; }
}

static void push_str(const char *s) { while (*s) kpush((uint8_t)*s++); }
static void key(uint8_t k, const char *vt) { if (gui) kpush(k); else push_str(vt); }

void input_push(uint8_t c) { push(c); }
void input_set_gui(int on) { gui = on; }

int input_gui_key(void)
{
    if (gtail == ghead) return -1;
    int c = gq[gtail];
    gtail = (gtail + 1) % QSIZE;
    return c;
}

int input_peek(void) { return qtail == qhead ? -1 : q[qtail]; }

int input_poll(void)
{
    if (qtail == qhead) return -1;
    int c = q[qtail];
    qtail = (qtail + 1) % QSIZE;
    return c;
}

int input_getc(void)
{
    int c;
    while ((c = input_poll()) < 0) __asm__ volatile("sti; hlt");
    return c;
}

/* ── COM1 ───────────────────────────────────────────────────────────── */
void input_serial_irq(void)
{
    if (!console_serial_present()) return;
    for (int i = 0; i < 64 && (inb(COM1 + 5) & 1); i++) push(inb(COM1));
}

/* ── PS/2 keyboard ──────────────────────────────────────────────────── */
static const char keymap[0x3A] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b', '\t',
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0, 'a', 's',
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\', 'z', 'x', 'c', 'v',
    'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ',
};
static const char keymap_shift[0x3A] = {
    0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b', '\t',
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0, 'A', 'S',
    'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|', 'Z', 'X', 'C', 'V',
    'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' ',
};

static int shift, ctrl, caps, ext;

void input_kbd_irq(void)
{
    uint8_t st = inb(0x64);
    if (!(st & 1)) return;
    uint8_t sc = inb(0x60);
    if (st & 0x20) { mouse_byte(sc); return; } /* a mouse byte that raced in: hand it over */

    if (sc == 0xE0) { ext = 1; return; }
    int up = sc & 0x80;
    sc &= 0x7F;

    if (ext) {
        ext = 0;
        if (sc == 0x1D) { ctrl = !up; return; }  /* right ctrl */
        if (up) return;
        switch (sc) {
        case 0x48: key(KEY_UP, "\033[A"); break;
        case 0x50: key(KEY_DOWN, "\033[B"); break;
        case 0x4D: key(KEY_RIGHT, "\033[C"); break;
        case 0x4B: key(KEY_LEFT, "\033[D"); break;
        case 0x47: key(KEY_HOME, "\033[H"); break;
        case 0x4F: key(KEY_END, "\033[F"); break;
        case 0x53: key(KEY_DEL, "\033[3~"); break;
        case 0x49: key(KEY_PGUP, "\033[5~"); break;
        case 0x51: key(KEY_PGDN, "\033[6~"); break;
        case 0x1C: kpush('\n'); break;          /* keypad enter */
        case 0x35: kpush('/'); break;           /* keypad / */
        case 0x5B: case 0x5C: kpush(KEY_SUPER); break;   /* Super / Windows key */
        }
        return;
    }

    if (sc == 0x2A || sc == 0x36) { shift = !up; return; }
    if (sc == 0x1D) { ctrl = !up; return; }
    if (up) return;
    if (sc == 0x3A) { caps = !caps; return; }
    if (sc >= sizeof keymap) return;

    if (sc == 0x0F && shift && gui) { kpush(KEY_SHIFT_TAB); return; }
    char c = shift ? keymap_shift[sc] : keymap[sc];
    if (!c) return;
    if (caps && c >= 'a' && c <= 'z' && !shift) c -= 32;
    else if (caps && c >= 'A' && c <= 'Z' && shift) c += 32;
    if (ctrl && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) c &= 0x1F;
    kpush((uint8_t)c);
}

void input_init(void)
{
    for (int i = 0; i < 32 && (inb(0x64) & 1); i++) inb(0x60);   /* drop anything left from the firmware */
    uint8_t unmask = 1 << 1;                   /* IRQ1: keyboard */
    if (console_serial_present()) {
        for (int i = 0; i < 64 && (inb(COM1 + 5) & 1); i++) inb(COM1);
        outb(COM1 + 1, 0x01);                  /* COM1: interrupt on received data */
        unmask |= 1 << 4;                      /* IRQ4: COM1 */
    }
    outb(0x21, inb(0x21) & (uint8_t)~unmask);
}

int input_shift(void) { return shift; }
int input_ctrl(void) { return ctrl; }
