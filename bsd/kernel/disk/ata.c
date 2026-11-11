/* IDE disks through the legacy ATA ports, PIO, LBA48 (QEMU's default disk). */
#include "kern.h"
#include "arch.h"
#include "rs.h"
#include "disk.h"

struct ata { uint16_t io, ctl; int slave; };

/* Waits are bounded by time (30 s on the PIT), with an iteration cap in case the
 * timer isn't running yet. A FLUSH CACHE after a large write can take seconds, and
 * the drive ignores register writes while it is busy, so giving up early sends the
 * next command to the wrong sector. */
static void delay400(uint16_t io) { for (int i = 0; i < 4; i++) (void)inb((uint16_t)(io + 7)); }
static int wait_bsy_s(uint16_t io, int secs)
{
    uint64_t end = kernel_ticks() + (uint64_t)secs * AUXO_HZ;
    for (uint64_t i = 0; i < (uint64_t)secs * 2000000ULL; i++) {
        uint8_t s = inb((uint16_t)(io + 7));
        if (!(s & 0x80)) return s;
        if ((i & 1023) == 1023 && kernel_ticks() > end) break;
    }
    return -1;
}
static int wait_drq_s(uint16_t io, int secs)
{
    uint64_t end = kernel_ticks() + (uint64_t)secs * AUXO_HZ;
    for (uint64_t i = 0; i < (uint64_t)secs * 2000000ULL; i++) {
        uint8_t s = inb((uint16_t)(io + 7));
        if (s & 0x01) return -1;              /* ERR */
        if (!(s & 0x80) && (s & 0x08)) return 0;
        if ((i & 1023) == 1023 && kernel_ticks() > end) break;
    }
    return -1;
}

static int wait_bsy(uint16_t io) { return wait_bsy_s(io, 30); }
static int wait_drq(uint16_t io) { return wait_drq_s(io, 30); }

static int setup(struct ata *a, uint64_t lba, uint16_t count, uint8_t cmd)
{
    uint16_t io = a->io;
    if (wait_bsy(io) < 0) { kprintf("ata: busy before cmd %x st=%x\n", cmd, inb((uint16_t)(io+7))); return -1; }
    outb((uint16_t)(io + 6), (uint8_t)(0x40 | (a->slave << 4)));
    delay400(io);
    int st = wait_bsy(io);
    if (st < 0 || (st & 0x08)) { kprintf("ata: after select cmd %x st=%x lba=%lu\n", cmd, inb((uint16_t)(io+7)), (unsigned long)lba); return -1; }        /* still busy, or stuck mid-transfer */
    outb((uint16_t)(io + 2), (uint8_t)(count >> 8));
    outb((uint16_t)(io + 3), (uint8_t)(lba >> 24));
    outb((uint16_t)(io + 4), (uint8_t)(lba >> 32));
    outb((uint16_t)(io + 5), (uint8_t)(lba >> 40));
    outb((uint16_t)(io + 2), (uint8_t)count);
    outb((uint16_t)(io + 3), (uint8_t)lba);
    outb((uint16_t)(io + 4), (uint8_t)(lba >> 8));
    outb((uint16_t)(io + 5), (uint8_t)(lba >> 16));
    outb((uint16_t)(io + 7), cmd);
    delay400(io);
    return 0;
}

static int ata_read(struct disk *d, uint64_t lba, uint32_t count, void *buf)
{
    struct ata *a = d->priv;
    uint8_t *b = buf;
    while (count) {
        uint16_t n = count > 256 ? 256 : (uint16_t)count;
        if (setup(a, lba, n, 0x24) < 0) return -5;                 /* READ SECTORS EXT */
        for (uint16_t i = 0; i < n; i++) {
            if (wait_drq(a->io) < 0) return -5;
            insw(a->io, b, 256);
            b += 512;
        }
        lba += n; count -= n;
    }
    return 0;
}

static int ata_write(struct disk *d, uint64_t lba, uint32_t count, const void *buf)
{
    struct ata *a = d->priv;
    const uint8_t *b = buf;
    while (count) {
        uint16_t n = count > 256 ? 256 : (uint16_t)count;
        if (setup(a, lba, n, 0x34) < 0) return -5;                 /* WRITE SECTORS EXT */
        for (uint16_t i = 0; i < n; i++) {
            if (wait_drq(a->io) < 0) { kprintf("ata: no drq on write lba=%lu i=%u st=%x\n", (unsigned long)lba, i, inb((uint16_t)(a->io+7))); return -5; }
            outsw(a->io, b, 256);
            b += 512;
        }
        lba += n; count -= n;
    }
    if (wait_bsy(a->io) < 0) return -5;
    outb((uint16_t)(a->io + 7), 0xEA);                         /* FLUSH CACHE EXT */
    delay400(a->io);
    int st = wait_bsy(a->io);
    if (st < 0 || (st & 0x01)) { kprintf("ata: flush st=%x\n", inb((uint16_t)(a->io+7))); return -5; }
    return 0;
}

int ata_probe(void)
{
    static const uint16_t ios[2] = { 0x1F0, 0x170 }, ctls[2] = { 0x3F6, 0x376 };
    static int seq;
    int found = 0;
    for (int ch = 0; ch < 2; ch++)
        for (int sl = 0; sl < 2; sl++) {
            uint16_t io = ios[ch];
            if (inb((uint16_t)(io + 7)) == 0xFF) continue;     /* floating bus: no controller */
            outb((uint16_t)(io + 6), (uint8_t)(0xA0 | (sl << 4)));
            for (int i = 0; i < 4; i++) inb(ctls[ch]);
            outb((uint16_t)(io + 2), 0); outb((uint16_t)(io + 3), 0); outb((uint16_t)(io + 4), 0); outb((uint16_t)(io + 5), 0);
            outb((uint16_t)(io + 7), 0xEC);                    /* IDENTIFY */
            if (!inb((uint16_t)(io + 7))) continue;
            if (wait_bsy_s(io, 1) < 0) continue;
            if (inb((uint16_t)(io + 4)) || inb((uint16_t)(io + 5))) continue;   /* ATAPI / SATA signature: not ours */
            if (wait_drq_s(io, 1) < 0) continue;
            uint16_t id[256];
            insw(io, id, 256);
            if (!(id[83] & (1 << 10))) continue;               /* LBA48 only */
            struct disk *d = rs_kmalloc(sizeof *d);
            struct ata *a = rs_kmalloc(sizeof *a);
            if (!d || !a) return found;
            memset(d, 0, sizeof *d);
            a->io = io; a->ctl = ctls[ch]; a->slave = sl;
            ksnprintf(d->name, sizeof d->name, "sd%c", 'a' + disk_count()); (void)seq;
            for (int i = 0; i < 20; i++) { d->model[2 * i] = (char)(id[27 + i] >> 8); d->model[2 * i + 1] = (char)id[27 + i]; }
            d->model[40] = 0;
            for (int i = 39; i >= 0 && d->model[i] == ' '; i--) d->model[i] = 0;
            strlcpy(d->bus, "IDE", sizeof d->bus);
            d->sectors = (uint64_t)id[100] | (uint64_t)id[101] << 16 | (uint64_t)id[102] << 32 | (uint64_t)id[103] << 48;
            d->read = ata_read; d->write = ata_write; d->priv = a;
            disk_add(d);
            found++;
        }
    return found;
}
