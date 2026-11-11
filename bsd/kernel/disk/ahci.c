/* SATA disks on an AHCI controller (VirtualBox's default, and most PCs). One command slot,
 * polled. */
#include "kern.h"
#include "arch.h"
#include "rs.h"
#include "pci.h"
#include "disk.h"

struct hba_port { volatile uint8_t *r; uint8_t *clb, *fb, *ct; };

#define P_CLB  0x00
#define P_FB   0x08
#define P_IS   0x10
#define P_CMD  0x18
#define P_TFD  0x20
#define P_SIG  0x24
#define P_SSTS 0x28
#define P_SERR 0x30
#define P_CI   0x38

static uint32_t rd(struct hba_port *p, uint32_t o) { return mmio_r32(p->r, o); }
static void wr(struct hba_port *p, uint32_t o, uint32_t v) { mmio_w32(p->r, o, v); }

/* one command: ATA `cmd` on `count` sectors at `lba`, data at buf */
static int issue(struct hba_port *p, uint8_t cmd, uint64_t lba, uint32_t count, void *buf, uint32_t bytes, int write)
{
    for (int i = 0; i < 50000000 && (rd(p, P_TFD) & 0x88); i++) __asm__ volatile("pause");
    if (rd(p, P_TFD) & 0x88) return -5;                    /* drive still busy: don't queue blind */
    wr(p, P_IS, 0xFFFFFFFF);
    uint32_t *hdr = (uint32_t *)p->clb;                    /* command header 0 */
    hdr[0] = 5 | (write ? 1u << 6 : 0) | (1u << 16);      /* FIS length 5 dwords, 1 PRD */
    hdr[1] = 0;
    uint64_t ct = (uintptr_t)p->ct;
    hdr[2] = (uint32_t)ct; hdr[3] = (uint32_t)(ct >> 32);
    memset(p->ct, 0, 256);
    uint8_t *fis = p->ct;
    fis[0] = 0x27; fis[1] = 0x80; fis[2] = cmd;
    fis[4] = (uint8_t)lba; fis[5] = (uint8_t)(lba >> 8); fis[6] = (uint8_t)(lba >> 16);
    fis[7] = cmd == 0xEC ? 0 : 0x40;
    fis[8] = (uint8_t)(lba >> 24); fis[9] = (uint8_t)(lba >> 32); fis[10] = (uint8_t)(lba >> 40);
    fis[12] = (uint8_t)count; fis[13] = (uint8_t)(count >> 8);
    uint32_t *prd = (uint32_t *)(p->ct + 0x80);
    uint64_t b = (uintptr_t)buf;
    prd[0] = (uint32_t)b; prd[1] = (uint32_t)(b >> 32); prd[2] = 0; prd[3] = bytes - 1;
    mb();
    wr(p, P_CI, 1);
    for (int i = 0; i < 50000000; i++) {
        if (!(rd(p, P_CI) & 1)) break;
        if (rd(p, P_IS) & (1u << 30)) return -5;           /* task file error */
        if ((i & 1023) == 1023) __asm__ volatile("pause");
    }
    if (rd(p, P_CI) & 1) return -5;
    if (rd(p, P_TFD) & 1) return -5;
    return 0;
}

static int a_rw(struct disk *d, uint64_t lba, uint32_t count, void *buf, int write)
{
    struct hba_port *p = d->priv;
    uint8_t *b = buf;
    while (count) {
        uint32_t n = count > 2048 ? 2048 : count;          /* 1 MiB per command */
        int r = issue(p, write ? 0x35 : 0x25, lba, n, b, n * 512, write);
        if (r < 0) return r;
        b += (size_t)n * 512; lba += n; count -= n;
    }
    if (write) issue(p, 0xEA, 0, 0, b, 512, 0);             /* FLUSH CACHE EXT */
    return 0;
}
static int a_read(struct disk *d, uint64_t lba, uint32_t count, void *buf) { return a_rw(d, lba, count, buf, 0); }
static int a_write(struct disk *d, uint64_t lba, uint32_t count, const void *buf) { return a_rw(d, lba, count, (void *)buf, 1); }

int ahci_probe(void)
{
    int found = 0;
    for (int k = 0; k < pci_count(); k++) {
        struct pci_dev *dev = pci_get(k);
        if (dev->class != 0x01 || dev->subclass != 0x06 || dev->prog_if != 0x01 || !dev->bar[5]) continue;
        volatile uint8_t *abar = vm_map_mmio(dev->bar[5], dev->barsize[5] ? dev->barsize[5] : 0x2000, 1);
        if (!abar) continue;
        pci_enable(dev, 1);
        dev->driver = "ahci";
        mmio_w32(abar, 0x04, mmio_r32(abar, 0x04) | 0x80000000u);   /* GHC.AE */
        uint32_t pi = mmio_r32(abar, 0x0C);
        for (int i = 0; i < 32; i++) {
            if (!(pi & (1u << i))) continue;
            struct hba_port tmp = { abar + 0x100 + i * 0x80, 0, 0, 0 };
            if ((rd(&tmp, P_SSTS) & 0xF) != 3) continue;          /* no device */
            if (rd(&tmp, P_SIG) != 0x00000101) continue;           /* not an ATA disk (ATAPI etc.) */
            struct hba_port *p = rs_kmalloc(sizeof *p);
            struct disk *d = rs_kmalloc(sizeof *d);
            uint64_t pg = pmm_alloc_contig(3);
            if (!p || !d || !pg) return found;
            *p = tmp;
            p->clb = (uint8_t *)(uintptr_t)pg; p->fb = p->clb + 4096; p->ct = p->clb + 8192;
            memset(p->clb, 0, 3 * 4096);
            /* stop, point it at our memory, start */
            wr(p, P_CMD, rd(p, P_CMD) & ~1u);
            for (int t = 0; t < 1000000 && (rd(p, P_CMD) & (1u << 15)); t++) { }
            wr(p, P_CMD, rd(p, P_CMD) & ~(1u << 4));
            for (int t = 0; t < 1000000 && (rd(p, P_CMD) & (1u << 14)); t++) { }
            uint64_t clb = (uintptr_t)p->clb, fb = (uintptr_t)p->fb;
            wr(p, P_CLB, (uint32_t)clb); wr(p, P_CLB + 4, (uint32_t)(clb >> 32));
            wr(p, P_FB, (uint32_t)fb); wr(p, P_FB + 4, (uint32_t)(fb >> 32));
            wr(p, P_SERR, 0xFFFFFFFF);
            wr(p, P_CMD, rd(p, P_CMD) | (1u << 4));
            wr(p, P_CMD, rd(p, P_CMD) | 1u);
            uint16_t *id = rs_kmalloc(512);
            if (!id) return found;
            if (issue(p, 0xEC, 0, 0, id, 512, 0) < 0) { rs_kfree(id); continue; }
            memset(d, 0, sizeof *d);
            ksnprintf(d->name, sizeof d->name, "sd%c", 'a' + disk_count());
            for (int j = 0; j < 20; j++) { d->model[2 * j] = (char)(id[27 + j] >> 8); d->model[2 * j + 1] = (char)id[27 + j]; }
            d->model[40] = 0;
            for (int j = 39; j >= 0 && d->model[j] == ' '; j--) d->model[j] = 0;
            strlcpy(d->bus, "SATA", sizeof d->bus);
            d->sectors = (uint64_t)id[100] | (uint64_t)id[101] << 16 | (uint64_t)id[102] << 32 | (uint64_t)id[103] << 48;
            rs_kfree(id);
            d->read = a_read; d->write = a_write; d->priv = p;
            disk_add(d);
            found++;
        }
    }
    return found;
}
