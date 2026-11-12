/* PS/2 mouse on the 8042's aux port, IRQ12. Tries the IntelliMouse handshake (sample rates
 * 200, 100, 80) for a scroll wheel. Packets are decoded into a small event queue. */
#include "kern.h"
#include "arch.h"

#define QN 64
static struct mouse_event q[QN];
static volatile int qh, qt;
static uint8_t pkt[4];
static int npkt, pktlen = 3, present;

static int wait_in(void)  { for (int i = 0; i < 100000; i++) if (!(inb(0x64) & 2)) return 1; return 0; }
static int wait_out(void) { for (int i = 0; i < 100000; i++) if (inb(0x64) & 1) return 1; return 0; }

static int aux_cmd(uint8_t c)
{
    if (!wait_in()) return -1;
    outb(0x64, 0xD4);
    if (!wait_in()) return -1;
    outb(0x60, c);
    if (!wait_out()) return -1;
    return inb(0x60);                                /* 0xFA = ACK */
}

int mouse_init(void)
{
    if (!wait_in()) return 0;
    outb(0x64, 0xA8);                                /* enable the aux port */
    if (!wait_in()) return 0;
    outb(0x64, 0x20);                                /* read the controller config */
    if (!wait_out()) return 0;
    uint8_t cfg = (uint8_t)((inb(0x60) | 0x02) & ~0x20);   /* aux IRQ on, aux clock on */
    wait_in(); outb(0x64, 0x60);
    wait_in(); outb(0x60, cfg);
    if (aux_cmd(0xF6) != 0xFA) return 0;             /* defaults */
    /* IntelliMouse: 200, 100, 80 → device id 3 means there's a wheel */
    uint8_t rates[3] = { 200, 100, 80 };
    for (int i = 0; i < 3; i++) { aux_cmd(0xF3); aux_cmd(rates[i]); }
    if (aux_cmd(0xF2) == 0xFA && wait_out() && inb(0x60) == 3) pktlen = 4;
    if (aux_cmd(0xF4) != 0xFA) return 0;             /* start streaming */
    for (int i = 0; i < 16 && (inb(0x64) & 1); i++) inb(0x60);
    present = 1;
    outb(0x21, inb(0x21) & (uint8_t)~(1 << 2));      /* cascade */
    outb(0xA1, inb(0xA1) & (uint8_t)~(1 << 4));      /* IRQ12 */
    return 1;
}

void mouse_byte(uint8_t b)
{
    if (npkt == 0 && !(b & 0x08)) return;            /* resync: bit 3 is always set in byte 0 */
    pkt[npkt++] = b;
    if (npkt < pktlen) return;
    npkt = 0;
    struct mouse_event e;
    e.buttons = pkt[0] & 7;
    e.dx = (int)pkt[1] - ((pkt[0] & 0x10) ? 256 : 0);
    e.dy = -((int)pkt[2] - ((pkt[0] & 0x20) ? 256 : 0));      /* screen y grows downwards */
    e.wheel = pktlen == 4 ? (int)(int8_t)(pkt[3] << 4) >> 4 : 0;
    if (pkt[0] & 0xC0) e.dx = e.dy = 0;              /* overflow */
    int n = (qh + 1) % QN;
    if (n != qt) { q[qh] = e; qh = n; }
}

void mouse_irq(void)
{
    for (int i = 0; i < 8; i++) {
        uint8_t st = inb(0x64);
        if ((st & 0x21) != 0x21) break;
        mouse_byte(inb(0x60));
    }
}

int mouse_poll(struct mouse_event *ev)
{
    if (!present || qt == qh) return 0;
    *ev = q[qt];
    qt = (qt + 1) % QN;
    return 1;
}

/* USB mice (usb/xhci.c), from a thread */
void mouse_usb_attached(void) { present = 1; }
void mouse_push(int dx, int dy, int wheel, int buttons)
{
    uint64_t f;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(f) :: "memory");
    struct mouse_event e = { dx, dy, wheel, buttons };
    int n = (qh + 1) % QN;
    if (n != qt) { q[qh] = e; qh = n; }
    present = 1;
    __asm__ volatile("push %0; popfq" :: "r"(f) : "memory", "cc");
}
