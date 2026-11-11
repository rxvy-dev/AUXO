/* Realtek RTL8169 / RTL8111 / RTL8168 gigabit Ethernet: the usual onboard chip on desktop
 * boards. Descriptor rings, polled, through the I/O port BAR. Written from Realtek's register
 * layout; this one has no emulator to test against (QEMU has no 8168), so it is best effort.
 * The 2.5 GbE RTL8125 is a different design and isn't claimed. */
#include "kern.h"
#include "arch.h"
#include "pci.h"
#include "net.h"

#define IDR0      0x00
#define TNPDS     0x20
#define CR        0x37
#define TPPOLL    0x38
#define IMR       0x3C
#define ISR       0x3E
#define TCR       0x40
#define RCR       0x44
#define CR9346    0x50
#define PHYSTATUS 0x6C
#define RMS       0xDA
#define CPCR      0xE0
#define RDSAR     0xE4
#define MTPS      0xEC

#define OWN (1u << 31)
#define EOR (1u << 30)
#define FS  (1u << 29)
#define LS  (1u << 28)

#define NRX 32
#define NTX 32
#define BUFSZ 2048

struct desc { uint32_t opts1, opts2, lo, hi; };

static struct {
    uint16_t io;
    struct desc *rx, *tx;
    uint8_t *rxbuf[NRX], *txbuf[NTX];
    int rxi, txi;
} rt;

static int r_send(struct nic *n, const void *frame, size_t len)
{
    (void)n;
    if (len > BUFSZ) return -1;
    struct desc *d = &rt.tx[rt.txi];
    for (int i = 0; i < 100000 && (d->opts1 & OWN); i++) __asm__ volatile("pause");
    if (d->opts1 & OWN) return -1;
    memcpy(rt.txbuf[rt.txi], frame, len);
    if (len < 60) { memset(rt.txbuf[rt.txi] + len, 0, 60 - len); len = 60; }
    d->opts2 = 0;
    mb();
    d->opts1 = OWN | FS | LS | (rt.txi == NTX - 1 ? EOR : 0) | (uint32_t)len;
    mb();
    outb((uint16_t)(rt.io + TPPOLL), 0x40);
    rt.txi = (rt.txi + 1) % NTX;
    return 0;
}

static int r_recv(struct nic *n, void *buf, size_t cap)
{
    (void)n;
    struct desc *d = &rt.rx[rt.rxi];
    uint32_t o = d->opts1;
    if (o & OWN) return 0;
    int len = (int)(o & 0x3FFF) - 4;                  /* minus the CRC */
    int ok = (o & FS) && (o & LS) && !(o & (1u << 21)) && len > 0 && (size_t)len <= cap;
    if (ok) memcpy(buf, rt.rxbuf[rt.rxi], (size_t)len);
    d->opts2 = 0;
    mb();
    d->opts1 = OWN | (rt.rxi == NRX - 1 ? EOR : 0) | BUFSZ;
    rt.rxi = (rt.rxi + 1) % NRX;
    outw((uint16_t)(rt.io + ISR), 0xFFFF);
    return ok ? len : 0;
}

static int r_link(struct nic *n) { (void)n; return (inb((uint16_t)(rt.io + PHYSTATUS)) & 2) != 0; }

int rtl8169_probe(struct pci_dev *d, struct nic *n)
{
    if (d->vendor != 0x10EC || (d->device != 0x8168 && d->device != 0x8169 && d->device != 0x8161 &&
                                d->device != 0x8167 && d->device != 0x8136))
        return -1;
    int io = -1;
    for (int i = 0; i < 6; i++) if (d->bario[i] && d->bar[i]) { io = i; break; }
    if (io < 0) return -1;
    rt.io = (uint16_t)d->bar[io];
    pci_enable(d, 1);

    outb((uint16_t)(rt.io + CR), 0x10);              /* reset */
    for (int i = 0; i < 1000 && (inb((uint16_t)(rt.io + CR)) & 0x10); i++) kdelay_ms(1);
    for (int i = 0; i < 6; i++) n->mac[i] = inb((uint16_t)(rt.io + IDR0 + i));

    rt.rx = (struct desc *)(uintptr_t)pmm_alloc_page();
    rt.tx = (struct desc *)(uintptr_t)pmm_alloc_page();
    if (!rt.rx || !rt.tx) return -1;
    memset(rt.rx, 0, PAGE_SIZE);
    memset(rt.tx, 0, PAGE_SIZE);
    for (int i = 0; i < NRX; i += 2) {
        uint8_t *pg = (uint8_t *)(uintptr_t)pmm_alloc_page();
        if (!pg) return -1;
        for (int k = 0; k < 2; k++) {
            rt.rxbuf[i + k] = pg + k * BUFSZ;
            rt.rx[i + k].lo = (uint32_t)(uintptr_t)rt.rxbuf[i + k];
            rt.rx[i + k].opts1 = OWN | (i + k == NRX - 1 ? EOR : 0) | BUFSZ;
        }
    }
    for (int i = 0; i < NTX; i += 2) {
        uint8_t *pg = (uint8_t *)(uintptr_t)pmm_alloc_page();
        if (!pg) return -1;
        for (int k = 0; k < 2; k++) {
            rt.txbuf[i + k] = pg + k * BUFSZ;
            rt.tx[i + k].lo = (uint32_t)(uintptr_t)rt.txbuf[i + k];
            rt.tx[i + k].opts1 = i + k == NTX - 1 ? EOR : 0;
        }
    }
    rt.rxi = rt.txi = 0;

    outb((uint16_t)(rt.io + CR9346), 0xC0);          /* unlock config registers */
    outw((uint16_t)(rt.io + CPCR), (uint16_t)(inw((uint16_t)(rt.io + CPCR)) | 0x08) & (uint16_t)~0x60);  /* PCI multi R/W, no VLAN/checksum offload */
    outw((uint16_t)(rt.io + RMS), BUFSZ);
    outb((uint16_t)(rt.io + MTPS), 0x3B);
    uint64_t tx = (uintptr_t)rt.tx, rx = (uintptr_t)rt.rx;
    outl((uint16_t)(rt.io + TNPDS), (uint32_t)tx); outl((uint16_t)(rt.io + TNPDS + 4), (uint32_t)(tx >> 32));
    outl((uint16_t)(rt.io + RDSAR), (uint32_t)rx); outl((uint16_t)(rt.io + RDSAR + 4), (uint32_t)(rx >> 32));
    outb((uint16_t)(rt.io + CR), 0x0C);              /* RX + TX on (before their configs, like Linux) */
    outl((uint16_t)(rt.io + TCR), (3u << 24) | (7u << 8));
    outl((uint16_t)(rt.io + RCR), (7u << 13) | (7u << 8) | 0x0E);   /* broadcast, multicast, our MAC */
    outw((uint16_t)(rt.io + IMR), 0);
    outw((uint16_t)(rt.io + ISR), 0xFFFF);
    outb((uint16_t)(rt.io + CR9346), 0x00);

    n->driver = "rtl8169";
    pci_describe(d, n->model, sizeof n->model);
    n->send = r_send; n->recv = r_recv; n->link = r_link;
    d->driver = n->driver;
    return 0;
}
