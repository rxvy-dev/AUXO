/* Intel PRO/1000 (e1000) and the e1000e family, legacy descriptors, polled.
 * Tested: 82540EM (QEMU's default NIC, VirtualBox's "Intel PRO/1000 MT Desktop").
 * The 82574L and the I217/I218/I219 onboard chips use the same rings and registers here;
 * they are claimed too, best effort (no ULP/PHY workarounds). */
#include "kern.h"
#include "arch.h"
#include "pci.h"
#include "net.h"

#define CTRL   0x0000
#define STATUS 0x0008
#define EERD   0x0014
#define ICR    0x00C0
#define IMC    0x00D8
#define RCTL   0x0100
#define TCTL   0x0400
#define TIPG   0x0410
#define RDBAL  0x2800
#define RDBAH  0x2804
#define RDLEN  0x2808
#define RDH    0x2810
#define RDT    0x2818
#define TDBAL  0x3800
#define TDBAH  0x3804
#define TDLEN  0x3808
#define TDH    0x3810
#define TDT    0x3818
#define MTA    0x5200
#define RAL    0x5400
#define RAH    0x5404

#define NRX 32
#define NTX 32
#define BUFSZ 2048

struct rxd { uint64_t addr; uint16_t len, csum; uint8_t status, errors; uint16_t special; } __attribute__((packed));
struct txd { uint64_t addr; uint16_t len; uint8_t cso, cmd, status, css; uint16_t special; } __attribute__((packed));

static struct e1000 {
    volatile uint8_t *mmio;
    struct rxd *rx;
    struct txd *tx;
    uint8_t *rxbuf[NRX], *txbuf[NTX];
    int rxi, txi, e1000e;
} card;

static const struct { uint16_t id; uint8_t e; } ids[] = {
    { 0x100E, 0 }, { 0x100F, 0 }, { 0x1004, 0 }, { 0x1026, 0 }, { 0x1076, 0 }, { 0x107C, 0 }, { 0x10F5, 1 },
    { 0x10D3, 1 }, { 0x10EA, 1 }, { 0x10EB, 1 }, { 0x1502, 1 }, { 0x1503, 1 }, { 0x153A, 1 }, { 0x153B, 1 },
    { 0x155A, 1 }, { 0x1559, 1 }, { 0x15A0, 1 }, { 0x15A1, 1 }, { 0x15A2, 1 }, { 0x15A3, 1 },
    { 0x156F, 1 }, { 0x1570, 1 }, { 0x15B7, 1 }, { 0x15B8, 1 }, { 0x15B9, 1 }, { 0x15BB, 1 }, { 0x15BC, 1 },
    { 0x15BD, 1 }, { 0x15BE, 1 }, { 0x15D6, 1 }, { 0x15D7, 1 }, { 0x15D8, 1 }, { 0x15E3, 1 }, { 0x15FA, 1 },
    { 0x15FB, 1 }, { 0x15FC, 1 }, { 0x0D4C, 1 }, { 0x0D4D, 1 }, { 0x0D4E, 1 }, { 0x0D4F, 1 }, { 0x0D53, 1 },
    { 0x0D55, 1 }, { 0x1A1C, 1 }, { 0x1A1D, 1 }, { 0x1A1E, 1 }, { 0x1A1F, 1 }, { 0x550A, 1 }, { 0x550B, 1 },
    { 0x550C, 1 }, { 0x550D, 1 }, { 0, 0 }
};

static uint32_t r(uint32_t o) { return mmio_r32(card.mmio, o); }
static void w(uint32_t o, uint32_t v) { mmio_w32(card.mmio, o, v); }

static uint16_t eeprom(int addr)
{
    /* 8254x: start bit 0, done bit 4, address << 8.  e1000e: done bit 1, address << 2 */
    if (card.e1000e) {
        w(EERD, (uint32_t)addr << 2 | 1);
        for (int i = 0; i < 100000; i++) { uint32_t v = r(EERD); if (v & 2) return (uint16_t)(v >> 16); }
    } else {
        w(EERD, (uint32_t)addr << 8 | 1);
        for (int i = 0; i < 100000; i++) { uint32_t v = r(EERD); if (v & 0x10) return (uint16_t)(v >> 16); }
    }
    return 0;
}

static int e_send(struct nic *n, const void *frame, size_t len)
{
    (void)n;
    if (len > BUFSZ) return -1;
    struct txd *d = &card.tx[card.txi];
    for (int i = 0; i < 100000 && !(d->status & 1); i++) __asm__ volatile("pause");
    if (!(d->status & 1)) return -1;                 /* ring full / card stuck */
    memcpy(card.txbuf[card.txi], frame, len);
    if (len < 60) { memset(card.txbuf[card.txi] + len, 0, 60 - len); len = 60; }
    d->len = (uint16_t)len;
    d->cmd = 0x0B;                                   /* EOP | IFCS | RS */
    d->status = 0;
    mb();
    card.txi = (card.txi + 1) % NTX;
    w(TDT, (uint32_t)card.txi);
    return 0;
}

static int e_recv(struct nic *n, void *buf, size_t cap)
{
    (void)n;
    struct rxd *d = &card.rx[card.rxi];
    if (!(d->status & 1)) return 0;
    size_t len = d->len;
    int ok = (d->status & 2) && !d->errors && len <= cap;
    if (ok) memcpy(buf, card.rxbuf[card.rxi], len);
    d->status = 0;
    mb();
    w(RDT, (uint32_t)card.rxi);
    card.rxi = (card.rxi + 1) % NRX;
    return ok ? (int)len : 0;
}

static int e_link(struct nic *n) { (void)n; return (r(STATUS) & 2) != 0; }

int e1000_probe(struct pci_dev *d, struct nic *n)
{
    if (d->vendor != 0x8086) return -1;
    int which = -1;
    for (int i = 0; ids[i].id; i++) if (ids[i].id == d->device) { which = i; break; }
    if (which < 0 || d->bario[0] || !d->bar[0]) return -1;
    card.e1000e = ids[which].e;
    card.mmio = vm_map_mmio(d->bar[0], d->barsize[0] ? d->barsize[0] : 0x20000, 1);
    if (!card.mmio) return -1;
    pci_enable(d, 1);

    w(IMC, 0xFFFFFFFF);
    w(CTRL, r(CTRL) | (1u << 26));                   /* reset */
    kdelay_ms(20);
    for (int i = 0; i < 1000 && (r(CTRL) & (1u << 26)); i++) kdelay_ms(1);
    w(IMC, 0xFFFFFFFF);
    (void)r(ICR);
    uint32_t ctrl = r(CTRL);
    ctrl |= (1u << 6) | (1u << 5);                   /* SLU, ASDE */
    ctrl &= ~((1u << 3) | (1u << 31) | (1u << 7) | (1u << 30));   /* LRST, PHY_RST, ILOS, VME */
    w(CTRL, ctrl);

    uint32_t lo = r(RAL), hi = r(RAH);
    if ((hi & 0x80000000u) && (lo || (hi & 0xFFFF))) {
        for (int i = 0; i < 4; i++) n->mac[i] = (uint8_t)(lo >> (8 * i));
        n->mac[4] = (uint8_t)hi; n->mac[5] = (uint8_t)(hi >> 8);
    } else {
        for (int i = 0; i < 3; i++) { uint16_t v = eeprom(i); n->mac[2 * i] = (uint8_t)v; n->mac[2 * i + 1] = (uint8_t)(v >> 8); }
        w(RAL, (uint32_t)n->mac[0] | (uint32_t)n->mac[1] << 8 | (uint32_t)n->mac[2] << 16 | (uint32_t)n->mac[3] << 24);
        w(RAH, (uint32_t)n->mac[4] | (uint32_t)n->mac[5] << 8 | 0x80000000u);
    }
    for (int i = 0; i < 128; i++) w(MTA + (uint32_t)i * 4, 0);

    /* rings: one page each for the descriptors, buffers two to a page */
    card.rx = (struct rxd *)(uintptr_t)pmm_alloc_page();
    card.tx = (struct txd *)(uintptr_t)pmm_alloc_page();
    if (!card.rx || !card.tx) return -1;
    memset(card.rx, 0, PAGE_SIZE);
    memset(card.tx, 0, PAGE_SIZE);
    for (int i = 0; i < NRX; i += 2) {
        uint8_t *pg = (uint8_t *)(uintptr_t)pmm_alloc_page();
        if (!pg) return -1;
        card.rxbuf[i] = pg; card.rxbuf[i + 1] = pg + BUFSZ;
        card.rx[i].addr = (uintptr_t)pg; card.rx[i + 1].addr = (uintptr_t)pg + BUFSZ;
    }
    for (int i = 0; i < NTX; i += 2) {
        uint8_t *pg = (uint8_t *)(uintptr_t)pmm_alloc_page();
        if (!pg) return -1;
        card.txbuf[i] = pg; card.txbuf[i + 1] = pg + BUFSZ;
        card.tx[i].addr = (uintptr_t)pg; card.tx[i + 1].addr = (uintptr_t)pg + BUFSZ;
        card.tx[i].status = card.tx[i + 1].status = 1;            /* free */
    }
    uint64_t rxp = (uintptr_t)card.rx, txp = (uintptr_t)card.tx;
    w(RDBAL, (uint32_t)rxp); w(RDBAH, (uint32_t)(rxp >> 32)); w(RDLEN, NRX * 16);
    w(RDH, 0); w(RDT, NRX - 1);
    card.rxi = 0;
    w(TDBAL, (uint32_t)txp); w(TDBAH, (uint32_t)(txp >> 32)); w(TDLEN, NTX * 16);
    w(TDH, 0); w(TDT, 0);
    card.txi = 0;
    w(RCTL, (1u << 1) | (1u << 15) | (1u << 26));                 /* EN, BAM, SECRC; 2048-byte buffers */
    w(TCTL, (1u << 1) | (1u << 3) | (0x0Fu << 4) | (0x3Fu << 12)); /* EN, PSP, CT, COLD */
    w(TIPG, card.e1000e ? 0x00602008 : 0x0060200A);

    n->driver = card.e1000e ? "e1000e" : "e1000";
    pci_describe(d, n->model, sizeof n->model);
    n->send = e_send; n->recv = e_recv; n->link = e_link;
    d->driver = n->driver;
    return 0;
}
