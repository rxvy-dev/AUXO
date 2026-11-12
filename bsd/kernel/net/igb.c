/* Intel's newer Ethernet chips: I210/I211 (igb, 1 Gbit, on many desktop boards), I350, the
 * 82576, and I225/I226 (igc, 2.5 Gbit, on most recent Intel and AMD boards). They share the
 * register layout used here: queue 0, advanced descriptors, polled. Tested on QEMU's 82576;
 * the igc chips get the same programming, best effort. */
#include "kern.h"
#include "arch.h"
#include "pci.h"
#include "net.h"

#define CTRL     0x0000
#define STATUS   0x0008
#define CTRL_EXT 0x0018
#define IMC      0x150C
#define ICR      0x1500
#define RCTL     0x0100
#define TCTL     0x0400
#define RDBAL    0xC000
#define RDBAH    0xC004
#define RDLEN    0xC008
#define SRRCTL   0xC00C
#define RDH      0xC010
#define RDT      0xC018
#define RXDCTL   0xC028
#define TDBAL    0xE000
#define TDBAH    0xE004
#define TDLEN    0xE008
#define TDH      0xE010
#define TDT      0xE018
#define TXDCTL   0xE028
#define MDIC     0x0020
#define MTA      0x5200
#define RAL      0x5400
#define RAH      0x5404

#define NRX 64
#define NTX 32
#define BUFSZ 2048

struct adesc { volatile uint64_t a, b; };          /* read: buffer, (len/cmd); write-back: status */

static struct igb {
    volatile uint8_t *mmio;
    struct adesc *rx, *tx;
    uint8_t *rxbuf[NRX], *txbuf[NTX];
    int rxi, txi, igc;
} card;

static const struct { uint16_t id; uint8_t igc; } ids[] = {
    { 0x10C9, 0 }, { 0x10E6, 0 }, { 0x10E7, 0 }, { 0x10E8, 0 }, { 0x1526, 0 }, { 0x150A, 0 }, { 0x1518, 0 }, /* 82576 */
    { 0x1521, 0 }, { 0x1522, 0 }, { 0x1523, 0 }, { 0x1524, 0 },                                               /* I350 */
    { 0x1533, 0 }, { 0x1536, 0 }, { 0x1537, 0 }, { 0x1538, 0 }, { 0x157B, 0 }, { 0x157C, 0 }, { 0x15F6, 0 }, /* I210 */
    { 0x1539, 0 },                                                                                            /* I211 */
    { 0x15F2, 1 }, { 0x15F3, 1 }, { 0x15F8, 1 }, { 0x15FD, 1 }, { 0x0D9F, 1 }, { 0x3100, 1 }, { 0x3101, 1 }, /* I225 */
    { 0x3102, 1 }, { 0x125B, 1 }, { 0x125C, 1 }, { 0x125D, 1 }, { 0x125F, 1 }, { 0x5502, 1 }, { 0x5503, 1 }, /* I226 */
    { 0, 0 }
};

static uint32_t r(uint32_t o) { return mmio_r32(card.mmio, o); }
static void w(uint32_t o, uint32_t v) { mmio_w32(card.mmio, o, v); }

/* MDIO write to the copper PHY (address 1): the PHY only starts negotiating, and the MAC only
 * reports link, once the driver restarts autonegotiation, as Linux's igb does at open. */
static void phy_write(uint32_t reg, uint16_t v)
{
    w(MDIC, v | reg << 16 | 1u << 21 | 1u << 26);
    for (int i = 0; i < 20 && !(r(MDIC) & (1u << 28)); i++) kdelay_ms(1);
}

static int g_send(struct nic *n, const void *frame, size_t len)
{
    (void)n;
    if (len > BUFSZ) return -1;
    struct adesc *d = &card.tx[card.txi];
    if (d->a && !(d->b & (1ULL << 32))) {                     /* still in use: wait a little */
        for (int i = 0; i < 100000 && !(d->b & (1ULL << 32)); i++) __asm__ volatile("pause");
        if (!(d->b & (1ULL << 32))) return -1;
    }
    memcpy(card.txbuf[card.txi], frame, len);
    if (len < 60) { memset(card.txbuf[card.txi] + len, 0, 60 - len); len = 60; }
    d->a = (uint64_t)(uintptr_t)card.txbuf[card.txi];
    /* DTALEN | DTYP=data(3) | DCMD = EOP, IFCS, RS, DEXT | PAYLEN */
    d->b = (uint64_t)len | 3ULL << 20 | (uint64_t)(0x01 | 0x02 | 0x08 | 0x20) << 24 | (uint64_t)len << 46;
    mb();
    card.txi = (card.txi + 1) % NTX;
    w(TDT, (uint32_t)card.txi);
    return 0;
}

static void rx_arm(int i)
{
    card.rx[i].a = (uint64_t)(uintptr_t)card.rxbuf[i];
    card.rx[i].b = 0;                                          /* (header buffer: none) */
}

static int g_recv(struct nic *n, void *buf, size_t cap)
{
    (void)n;
    struct adesc *d = &card.rx[card.rxi];
    uint32_t st = (uint32_t)d->b;
    if (!(st & 1)) return 0;                                   /* DD */
    mb();
    size_t len = (size_t)(d->b >> 32) & 0xFFFF;
    int ok = (st & 2) && !(st >> 20 & 0xFF) && len <= cap;    /* EOP, no errors */
    if (ok) memcpy(buf, card.rxbuf[card.rxi], len);
    rx_arm(card.rxi);
    w(RDT, (uint32_t)card.rxi);
    card.rxi = (card.rxi + 1) % NRX;
    return ok ? (int)len : 0;
}

static int g_link(struct nic *n) { (void)n; return (r(STATUS) & 2) != 0; }

int igb_probe(struct pci_dev *d, struct nic *n)
{
    if (d->vendor != 0x8086) return -1;
    int which = -1;
    for (int i = 0; ids[i].id; i++) if (ids[i].id == d->device) { which = i; break; }
    if (which < 0 || d->bario[0] || !d->bar[0]) return -1;
    card.igc = ids[which].igc;
    card.mmio = vm_map_mmio(d->bar[0], d->barsize[0] ? d->barsize[0] : 0x20000, 1);
    if (!card.mmio) return -1;
    pci_enable(d, 1);

    w(IMC, 0xFFFFFFFF);
    w(RCTL, 0); w(TCTL, 0);
    w(CTRL, r(CTRL) | (1u << 26));                             /* reset */
    kdelay_ms(20);
    for (int i = 0; i < 1000 && (r(CTRL) & (1u << 26)); i++) kdelay_ms(1);
    w(IMC, 0xFFFFFFFF);
    (void)r(ICR);
    w(CTRL_EXT, r(CTRL_EXT) | (1u << 28));                     /* DRV_LOAD: the firmware leaves it to us */
    w(CTRL, (r(CTRL) | (1u << 6)) & ~((1u << 3) | (1u << 31))); /* SLU; not LRST, PHY_RST */

    phy_write(0, 0x1340);                                      /* BMCR: autoneg on + restart, 1000/full */

    uint32_t lo = r(RAL), hi = r(RAH);
    if (!lo && !(hi & 0xFFFF)) return -1;                      /* no address loaded from its NVM */
    for (int i = 0; i < 4; i++) n->mac[i] = (uint8_t)(lo >> (8 * i));
    n->mac[4] = (uint8_t)hi; n->mac[5] = (uint8_t)(hi >> 8);
    w(RAH, hi | 0x80000000u);                                  /* address valid */
    for (int i = 0; i < 128; i++) w(MTA + (uint32_t)i * 4, 0);

    card.rx = (struct adesc *)(uintptr_t)pmm_alloc_page();
    card.tx = (struct adesc *)(uintptr_t)pmm_alloc_page();
    if (!card.rx || !card.tx) return -1;
    memset(card.rx, 0, PAGE_SIZE); memset(card.tx, 0, PAGE_SIZE);
    for (int i = 0; i < NRX; i += 2) {
        uint8_t *pg = (uint8_t *)(uintptr_t)pmm_alloc_page();
        if (!pg) return -1;
        card.rxbuf[i] = pg; card.rxbuf[i + 1] = pg + BUFSZ;
        rx_arm(i); rx_arm(i + 1);
    }
    for (int i = 0; i < NTX; i += 2) {
        uint8_t *pg = (uint8_t *)(uintptr_t)pmm_alloc_page();
        if (!pg) return -1;
        card.txbuf[i] = pg; card.txbuf[i + 1] = pg + BUFSZ;
    }
    uint64_t rxp = (uintptr_t)card.rx, txp = (uintptr_t)card.tx;
    w(RXDCTL, 0);
    w(RDBAL, (uint32_t)rxp); w(RDBAH, (uint32_t)(rxp >> 32)); w(RDLEN, NRX * 16);
    w(SRRCTL, (BUFSZ / 1024) | 1u << 25);                      /* 2 KiB buffers, advanced one-buffer */
    w(RDH, 0); w(RDT, 0);
    w(RXDCTL, r(RXDCTL) | 1u << 25);                           /* queue enable */
    for (int i = 0; i < 100 && !(r(RXDCTL) & 1u << 25); i++) kdelay_ms(1);
    w(RDT, NRX - 1);
    card.rxi = 0;
    w(TXDCTL, 0);
    w(TDBAL, (uint32_t)txp); w(TDBAH, (uint32_t)(txp >> 32)); w(TDLEN, NTX * 16);
    w(TDH, 0); w(TDT, 0);
    w(TXDCTL, r(TXDCTL) | 1u << 25);
    for (int i = 0; i < 100 && !(r(TXDCTL) & 1u << 25); i++) kdelay_ms(1);
    card.txi = 0;
    w(RCTL, (1u << 1) | (1u << 15) | (1u << 26));              /* EN, BAM, SECRC */
    w(TCTL, (1u << 1) | (1u << 3) | (0x0Fu << 4) | (0x3Fu << 12));   /* EN, PSP, CT, COLD */

    n->driver = card.igc ? "igc" : "igb";
    pci_describe(d, n->model, sizeof n->model);
    n->send = g_send; n->recv = g_recv; n->link = g_link;
    d->driver = n->driver;
    return 0;
}
