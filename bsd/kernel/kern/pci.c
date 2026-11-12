/* PCI bus scan and names. Configuration mechanism #1 (ports 0xCF8/0xCFC) reaches the first
 * 256 bytes of every function, which is all the drivers here need. */
#include "kern.h"
#include "arch.h"
#include "pci.h"

#define MAXDEV 64
static struct pci_dev devs[MAXDEV];
static int ndev;

static uint32_t cfg_addr(int bus, int dev, int fn, uint8_t off)
{
    return 0x80000000u | (uint32_t)bus << 16 | (uint32_t)dev << 11 | (uint32_t)fn << 8 | (off & 0xFC);
}
static uint32_t rd(int bus, int dev, int fn, uint8_t off) { outl(0xCF8, cfg_addr(bus, dev, fn, off)); return inl(0xCFC); }
static void wr(int bus, int dev, int fn, uint8_t off, uint32_t v) { outl(0xCF8, cfg_addr(bus, dev, fn, off)); outl(0xCFC, v); }

uint32_t pci_read32(const struct pci_dev *d, uint8_t off) { return rd(d->bus, d->dev, d->fn, off); }
void pci_write32(const struct pci_dev *d, uint8_t off, uint32_t v) { wr(d->bus, d->dev, d->fn, off, v); }
uint16_t pci_read16(const struct pci_dev *d, uint8_t off) { return (uint16_t)(pci_read32(d, off & 0xFC) >> ((off & 2) * 8)); }
void pci_write16(const struct pci_dev *d, uint8_t off, uint16_t v)
{
    uint32_t x = pci_read32(d, off & 0xFC), sh = (off & 2) * 8;
    pci_write32(d, off & 0xFC, (x & ~(0xFFFFu << sh)) | (uint32_t)v << sh);
}

void pci_enable(struct pci_dev *d, int bus_master)
{
    uint16_t cmd = pci_read16(d, 0x04);
    cmd |= 0x3;                                  /* I/O + memory space */
    if (bus_master) cmd |= 0x4;
    cmd &= (uint16_t)~0x400;                     /* legacy INTx stays enabled; we poll anyway */
    pci_write16(d, 0x04, cmd);
}

static void read_bars(struct pci_dev *d)
{
    int nbars = (rd(d->bus, d->dev, d->fn, 0x0C) >> 16 & 0x7F) == 0 ? 6 : 2;   /* bridges have 2 */
    uint16_t cmd = pci_read16(d, 0x04);
    pci_write16(d, 0x04, cmd & (uint16_t)~0x3);  /* decode off while sizing */
    for (int i = 0; i < nbars; i++) {
        uint8_t off = (uint8_t)(0x10 + i * 4);
        uint32_t v = pci_read32(d, off);
        pci_write32(d, off, 0xFFFFFFFF);
        uint32_t sz = pci_read32(d, off);
        pci_write32(d, off, v);
        if (v & 1) {                             /* I/O */
            d->bario[i] = 1;
            d->bar[i] = v & ~3u;
            d->barsize[i] = sz ? (~(sz & ~3u) + 1) & 0xFFFF : 0;
            continue;
        }
        uint64_t base = v & ~0xFu, size = sz & ~0xFu;
        if (((v >> 1) & 3) == 2 && i + 1 < nbars) {   /* 64-bit */
            uint32_t hi = pci_read32(d, (uint8_t)(off + 4));
            pci_write32(d, (uint8_t)(off + 4), 0xFFFFFFFF);
            uint32_t szhi = pci_read32(d, (uint8_t)(off + 4));
            pci_write32(d, (uint8_t)(off + 4), hi);
            base |= (uint64_t)hi << 32;
            uint64_t s64 = (uint64_t)szhi << 32 | size;
            d->bar[i] = base;
            d->barsize[i] = s64 ? ~s64 + 1 : 0;
            i++;
            continue;
        }
        d->bar[i] = base;
        d->barsize[i] = size ? (uint32_t)(~size + 1) : 0;
    }
    pci_write16(d, 0x04, cmd);
}

static void add(int bus, int dev, int fn)
{
    uint32_t id = rd(bus, dev, fn, 0);
    if ((id & 0xFFFF) == 0xFFFF || ndev >= MAXDEV) return;
    struct pci_dev *d = &devs[ndev++];
    memset(d, 0, sizeof *d);
    d->bus = (uint8_t)bus; d->dev = (uint8_t)dev; d->fn = (uint8_t)fn;
    d->vendor = (uint16_t)id; d->device = (uint16_t)(id >> 16);
    uint32_t cl = rd(bus, dev, fn, 0x08);
    d->rev = (uint8_t)cl; d->prog_if = (uint8_t)(cl >> 8); d->subclass = (uint8_t)(cl >> 16); d->class = (uint8_t)(cl >> 24);
    d->irq = (uint8_t)rd(bus, dev, fn, 0x3C);
    read_bars(d);
}

void pci_init(void)
{
    ndev = 0;
    for (int bus = 0; bus < 256; bus++)
        for (int dev = 0; dev < 32; dev++) {
            uint32_t id = rd(bus, dev, 0, 0);
            if ((id & 0xFFFF) == 0xFFFF) continue;
            add(bus, dev, 0);
            if (rd(bus, dev, 0, 0x0C) & 0x00800000)       /* multi-function */
                for (int fn = 1; fn < 8; fn++) add(bus, dev, fn);
        }
}

int pci_count(void) { return ndev; }
struct pci_dev *pci_get(int i) { return i >= 0 && i < ndev ? &devs[i] : 0; }

struct pci_dev *pci_find_class(int class, int subclass, int start)
{
    for (int i = start < 0 ? 0 : start; i < ndev; i++)
        if (devs[i].class == class && (subclass < 0 || devs[i].subclass == subclass)) return &devs[i];
    return 0;
}

const struct pci_dev *pci_gpu(void)
{
    /* the card the screen is on: the one whose memory BAR holds the boot framebuffer. On a
     * laptop with Intel or AMD graphics plus an NVIDIA chip, that's the integrated one. */
    uint64_t fbp = fb_phys();
    if (fbp)
        for (int i = 0; i < ndev; i++)
            if (devs[i].class == 0x03)
                for (int b = 0; b < 6; b++)
                    if (devs[i].bar[b] && devs[i].barsize[b] && fbp >= devs[i].bar[b] && fbp < devs[i].bar[b] + devs[i].barsize[b])
                        return &devs[i];
    /* prefer a discrete card (NVIDIA/AMD) over the integrated one when both are there */
    const struct pci_dev *any = 0;
    for (int i = 0; i < ndev; i++)
        if (devs[i].class == 0x03) {
            if (devs[i].vendor == 0x10DE || devs[i].vendor == 0x1002) return &devs[i];
            if (!any) any = &devs[i];
        }
    return any;
}

const struct pci_dev *pci_wifi(void)
{
    for (int i = 0; i < ndev; i++)
        if (devs[i].class == 0x02 && devs[i].subclass == 0x80) return &devs[i];
    return 0;
}

const char *pci_vendor_name(uint16_t v)
{
    switch (v) {
    case 0x10DE: return "NVIDIA";
    case 0x1002: return "AMD";
    case 0x8086: return "Intel";
    case 0x10EC: return "Realtek";
    case 0x14E4: return "Broadcom";
    case 0x14C3: return "MediaTek";
    case 0x168C: case 0x17CB: return "Qualcomm";
    case 0x1234: return "QEMU";
    case 0x15AD: return "VMware";
    case 0x80EE: return "VirtualBox";
    case 0x1AF4: return "Virtio";
    case 0x1B36: return "Red Hat";
    case 0x1022: return "AMD";
    case 0x144D: return "Samsung";
    case 0x1E0F: return "KIOXIA";
    case 0x15B7: return "WD";
    case 0x1987: return "Phison";
    case 0x1C5C: return "SK hynix";
    case 0x126F: return "Silicon Motion";
    case 0x1B21: return "ASMedia";
    case 0x1106: return "VIA";
    default:     return 0;
    }
}

const char *pci_class_name(uint8_t c, uint8_t s)
{
    switch (c) {
    case 0x01:
        switch (s) { case 0x01: return "IDE controller"; case 0x06: return "SATA controller";
                     case 0x08: return "NVMe controller"; default: return "storage controller"; }
    case 0x02: return s == 0x80 ? "Wi-Fi" : s == 0x00 ? "Ethernet" : "network controller";
    case 0x03: return s == 0x00 ? "VGA" : s == 0x02 ? "3D controller" : "display controller";
    case 0x04: return s == 0x03 ? "audio" : "multimedia";
    case 0x06: return s == 0x00 ? "host bridge" : s == 0x01 ? "ISA bridge" : s == 0x04 ? "PCI bridge" : "bridge";
    case 0x0C:
        if (s == 0x03) return "USB controller";
        if (s == 0x05) return "SMBus";
        return "serial bus";
    case 0x0D: return "wireless";
    default: return "device";
    }
}

/* ── names for the hardware people ask about ────────────────────────── */
struct idname { uint16_t vendor, device; const char *name; };
static const struct idname known[] = {
    /* NVIDIA GeForce: Blackwell, Ada, Ampere, Turing (desktop) */
    { 0x10DE, 0x2B85, "GeForce RTX 5090" },     { 0x10DE, 0x2C02, "GeForce RTX 5080" },
    { 0x10DE, 0x2C05, "GeForce RTX 5070 Ti" },  { 0x10DE, 0x2F04, "GeForce RTX 5070" },
    { 0x10DE, 0x2D04, "GeForce RTX 5060 Ti" },  { 0x10DE, 0x2D05, "GeForce RTX 5060" },
    { 0x10DE, 0x2684, "GeForce RTX 4090" },     { 0x10DE, 0x2702, "GeForce RTX 4080 SUPER" },
    { 0x10DE, 0x2704, "GeForce RTX 4080" },     { 0x10DE, 0x2705, "GeForce RTX 4070 Ti SUPER" },
    { 0x10DE, 0x2782, "GeForce RTX 4070 Ti" },  { 0x10DE, 0x2783, "GeForce RTX 4070 SUPER" },
    { 0x10DE, 0x2786, "GeForce RTX 4070" },     { 0x10DE, 0x2803, "GeForce RTX 4060 Ti" },
    { 0x10DE, 0x2805, "GeForce RTX 4060 Ti" },  { 0x10DE, 0x2882, "GeForce RTX 4060" },
    { 0x10DE, 0x2204, "GeForce RTX 3090" },     { 0x10DE, 0x2203, "GeForce RTX 3090 Ti" },
    { 0x10DE, 0x2206, "GeForce RTX 3080" },     { 0x10DE, 0x2208, "GeForce RTX 3080 Ti" },
    { 0x10DE, 0x2484, "GeForce RTX 3070" },     { 0x10DE, 0x2482, "GeForce RTX 3070 Ti" },
    { 0x10DE, 0x2486, "GeForce RTX 3060 Ti" },  { 0x10DE, 0x2489, "GeForce RTX 3060 Ti" },
    { 0x10DE, 0x2503, "GeForce RTX 3060" },     { 0x10DE, 0x2504, "GeForce RTX 3060" },
    { 0x10DE, 0x1E04, "GeForce RTX 2080 Ti" },  { 0x10DE, 0x1E82, "GeForce RTX 2080" },
    { 0x10DE, 0x1F02, "GeForce RTX 2070" },     { 0x10DE, 0x1F08, "GeForce RTX 2060" },
    { 0x10DE, 0x2184, "GeForce GTX 1660" },     { 0x10DE, 0x1B80, "GeForce GTX 1080" },
    { 0x10DE, 0x1B81, "GeForce GTX 1070" },     { 0x10DE, 0x1C82, "GeForce GTX 1050 Ti" },
    /* AMD Radeon: RDNA 4, 3, 2 */
    { 0x1002, 0x7550, "Radeon RX 9070 / 9070 XT" }, { 0x1002, 0x7590, "Radeon RX 9060 XT" },
    { 0x1002, 0x744C, "Radeon RX 7900 XT/XTX" },    { 0x1002, 0x7448, "Radeon Pro W7900" },
    { 0x1002, 0x747E, "Radeon RX 7800 XT / 7700 XT" }, { 0x1002, 0x7480, "Radeon RX 7600" },
    { 0x1002, 0x73AF, "Radeon RX 6900 XT" },        { 0x1002, 0x73BF, "Radeon RX 6800 / 6900 XT" },
    { 0x1002, 0x73DF, "Radeon RX 6700 XT" },        { 0x1002, 0x73FF, "Radeon RX 6600 / 6600 XT" },
    { 0x1002, 0x731F, "Radeon RX 5700 / 5700 XT" }, { 0x1002, 0x67DF, "Radeon RX 470/480/570/580" },
    { 0x1002, 0x164E, "Radeon Graphics (Ryzen 7000)" }, { 0x1002, 0x15BF, "Radeon 780M" },
    { 0x1002, 0x1681, "Radeon 680M" },              { 0x1002, 0x1638, "Radeon Vega (Ryzen 5000)" },
    { 0x1002, 0x150E, "Radeon 890M" },
    /* Intel graphics: Arc, Xe, UHD */
    { 0x8086, 0xE20B, "Arc B580" },                 { 0x8086, 0xE20C, "Arc B570" },
    { 0x8086, 0x56A0, "Arc A770" },                 { 0x8086, 0x56A1, "Arc A750" },
    { 0x8086, 0x56A5, "Arc A380" },                 { 0x8086, 0x7D55, "Arc Graphics (Core Ultra)" },
    { 0x8086, 0x64A0, "Arc 140V" },                 { 0x8086, 0xA7A0, "Iris Xe Graphics" },
    { 0x8086, 0x9A49, "Iris Xe Graphics" },         { 0x8086, 0x46A6, "Iris Xe Graphics" },
    { 0x8086, 0xA780, "UHD Graphics 770" },         { 0x8086, 0x4680, "UHD Graphics 770" },
    { 0x8086, 0x4692, "UHD Graphics 730" },         { 0x8086, 0x9BC5, "UHD Graphics 630" },
    { 0x8086, 0x3E92, "UHD Graphics 630" },         { 0x8086, 0x3E9B, "UHD Graphics 630" },
    { 0x8086, 0x5912, "HD Graphics 630" },          { 0x8086, 0x0412, "HD Graphics 4600" },
    /* virtual GPUs */
    { 0x1234, 0x1111, "Standard VGA (Bochs)" }, { 0x15AD, 0x0405, "SVGA II" },
    { 0x80EE, 0xBEEF, "VirtualBox Graphics" },  { 0x1B36, 0x0100, "QXL" },
    { 0x1AF4, 0x1050, "virtio-gpu" },
    /* Ethernet with a driver here */
    { 0x8086, 0x100E, "82540EM Gigabit" },      { 0x8086, 0x100F, "82545EM Gigabit" },
    { 0x8086, 0x10D3, "82574L Gigabit" },       { 0x8086, 0x153A, "I217-LM" },
    { 0x8086, 0x15B8, "I219-V" },               { 0x8086, 0x15BC, "I219-V" },
    { 0x8086, 0x15FA, "I219-V" },               { 0x8086, 0x1A1F, "I219-V" },
    { 0x10EC, 0x8168, "RTL8111/8168 Gigabit" }, { 0x10EC, 0x8169, "RTL8169 Gigabit" },
    { 0x10EC, 0x8161, "RTL8111 Gigabit" },      { 0x10EC, 0x8125, "RTL8125 2.5GbE" },
    { 0x10EC, 0x8139, "RTL8139" },              { 0x8086, 0x15F3, "I225-V 2.5GbE" },
    { 0x8086, 0x125C, "I226-V 2.5GbE" },
    /* Wi-Fi */
    { 0x8086, 0x2723, "Wi-Fi 6 AX200" },        { 0x8086, 0x2725, "Wi-Fi 6E AX210" },
    { 0x8086, 0x272B, "Wi-Fi 7 BE200" },        { 0x8086, 0x51F0, "Wi-Fi 6E AX211" },
    { 0x8086, 0x7AF0, "Wi-Fi 6E AX211" },       { 0x8086, 0xA0F0, "Wi-Fi 6 AX201" },
    { 0x8086, 0x02F0, "Wi-Fi 6 AX201" },        { 0x8086, 0x06F0, "Wi-Fi 6 AX201" },
    { 0x8086, 0x2526, "Wireless-AC 9260" },     { 0x10EC, 0xC822, "RTL8822CE Wi-Fi" },
    { 0x10EC, 0xC821, "RTL8821CE Wi-Fi" },      { 0x10EC, 0xB852, "RTL8852BE Wi-Fi 6" },
    { 0x10EC, 0x8852, "RTL8852AE Wi-Fi 6" },    { 0x10EC, 0xC852, "RTL8852CE Wi-Fi 6E" },
    { 0x14C3, 0x7961, "MT7921 Wi-Fi 6" },       { 0x14C3, 0x0616, "MT7922 Wi-Fi 6E" },
    { 0x14C3, 0x7925, "MT7925 Wi-Fi 7" },       { 0x17CB, 0x1103, "WCN6855 Wi-Fi 6E" },
    { 0x168C, 0x003E, "QCA6174 Wi-Fi 5" },
    { 0, 0, 0 }
};

static const char *nvidia_family(uint16_t id)
{
    if (id >= 0x2B00 && id < 0x3000) return "GeForce RTX 50 series (Blackwell)";
    if (id >= 0x2600 && id < 0x2B00) return "GeForce RTX 40 series (Ada)";
    if (id >= 0x2200 && id < 0x2600) return "GeForce RTX 30 series (Ampere)";
    if (id >= 0x1E00 && id < 0x2200) return "GeForce RTX 20 / GTX 16 series (Turing)";
    if (id >= 0x1B00 && id < 0x1E00) return "GeForce GTX 10 series (Pascal)";
    return "GPU";
}

void pci_describe(const struct pci_dev *d, char *buf, int n)
{
    const char *v = pci_vendor_name(d->vendor);
    for (const struct idname *k = known; k->name; k++)
        if (k->vendor == d->vendor && k->device == d->device) {
            ksnprintf(buf, (size_t)n, "%s %s", v ? v : "", k->name);
            return;
        }
    if (d->vendor == 0x10DE && d->class == 0x03) { ksnprintf(buf, (size_t)n, "NVIDIA %s", nvidia_family(d->device)); return; }
    if (v) ksnprintf(buf, (size_t)n, "%s %s", v, pci_class_name(d->class, d->subclass));
    else ksnprintf(buf, (size_t)n, "%s [%04x:%04x]", pci_class_name(d->class, d->subclass), d->vendor, d->device);
}
