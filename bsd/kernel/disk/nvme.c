/* NVMe SSDs: one admin queue, one I/O queue, polled; 512-byte-sector namespaces. */
#include "kern.h"
#include "arch.h"
#include "rs.h"
#include "pci.h"
#include "disk.h"

#define QN 64
struct nvme {
    volatile uint8_t *r;
    uint32_t dstrd;
    uint8_t *asq, *acq, *iosq, *iocq, *prp;
    uint16_t asq_t, acq_h, iosq_t, iocq_h;
    int aphase, iophase;
    uint16_t cid;
    uint32_t nsid;
};

static void db(struct nvme *n, int q, int cq, uint32_t v) { mmio_w32(n->r, 0x1000 + (uint32_t)(2 * q + cq) * (4u << n->dstrd), v); }

/* submit a 64-byte command on queue (admin or I/O), wait for its completion; status or -1 */
static int submit(struct nvme *n, int io, uint32_t *cmd)
{
    uint8_t *sq = io ? n->iosq : n->asq, *cq = io ? n->iocq : n->acq;
    uint16_t *tail = io ? &n->iosq_t : &n->asq_t, *head = io ? &n->iocq_h : &n->acq_h;
    int *phase = io ? &n->iophase : &n->aphase;
    cmd[0] = (cmd[0] & 0xFFFF) | ((uint32_t)++n->cid << 16);
    memcpy(sq + (size_t)*tail * 64, cmd, 64);
    *tail = (uint16_t)((*tail + 1) % QN);
    mb();
    db(n, io, 0, *tail);
    volatile uint32_t *e = (volatile uint32_t *)(cq + (size_t)*head * 16);
    for (uint64_t i = 0; i < 400000000ULL; i++) {
        if (((e[3] >> 16) & 1) == (uint32_t)*phase) {
            int status = (int)((e[3] >> 17) & 0x7FFF);
            *head = (uint16_t)((*head + 1) % QN);
            if (!*head) *phase ^= 1;
            db(n, io, 1, *head);
            return status;
        }
        __asm__ volatile("pause");
    }
    return -1;
}

static int rw(struct disk *d, uint64_t lba, uint32_t count, void *buf, int write)
{
    struct nvme *n = d->priv;
    uint8_t *b = buf;
    while (count) {
        uint32_t c = count > 128 ? 128 : count;              /* 64 KiB: one PRP list page covers it */
        uint64_t start = (uintptr_t)b, end = start + (uint64_t)c * 512;
        uint32_t cmd[16] = { 0 };
        cmd[0] = write ? 0x01 : 0x02;
        cmd[1] = n->nsid;
        cmd[6] = (uint32_t)start; cmd[7] = (uint32_t)(start >> 32);
        uint64_t next = (start & ~0xFFFULL) + 4096;
        if (end > next) {
            if (end <= next + 4096) { cmd[8] = (uint32_t)next; cmd[9] = (uint32_t)(next >> 32); }
            else {
                uint64_t *list = (uint64_t *)n->prp;
                int k = 0;
                for (uint64_t a = next; a < end; a += 4096) list[k++] = a;
                uint64_t lp = (uintptr_t)n->prp;
                cmd[8] = (uint32_t)lp; cmd[9] = (uint32_t)(lp >> 32);
            }
        }
        cmd[10] = (uint32_t)lba; cmd[11] = (uint32_t)(lba >> 32); cmd[12] = c - 1;
        if (submit(n, 1, cmd) != 0) return -5;
        b += (size_t)c * 512; lba += c; count -= c;
    }
    if (write) { uint32_t cmd[16] = { 0x00 }; cmd[1] = n->nsid; submit(n, 1, cmd); }   /* FLUSH */
    return 0;
}
static int n_read(struct disk *d, uint64_t lba, uint32_t c, void *b) { return rw(d, lba, c, b, 0); }
static int n_write(struct disk *d, uint64_t lba, uint32_t c, const void *b) { return rw(d, lba, c, (void *)b, 1); }

int nvme_probe(void)
{
    int found = 0, seq = 0;
    for (int k = 0; k < pci_count(); k++) {
        struct pci_dev *dev = pci_get(k);
        if (dev->class != 0x01 || dev->subclass != 0x08 || dev->prog_if != 0x02 || !dev->bar[0]) continue;
        struct nvme *n = rs_kmalloc(sizeof *n);
        uint64_t mem = pmm_alloc_contig(6);
        if (!n || !mem) return found;
        memset(n, 0, sizeof *n);
        memset((void *)(uintptr_t)mem, 0, 6 * 4096);
        n->r = vm_map_mmio(dev->bar[0], dev->barsize[0] ? dev->barsize[0] : 0x4000, 1);
        if (!n->r) continue;
        pci_enable(dev, 1);
        dev->driver = "nvme";
        uint8_t *m = (uint8_t *)(uintptr_t)mem;
        n->asq = m; n->acq = m + 4096; n->iosq = m + 8192; n->iocq = m + 12288; n->prp = m + 16384;
        uint8_t *idbuf = m + 20480;
        uint64_t cap = (uint64_t)mmio_r32(n->r, 0) | (uint64_t)mmio_r32(n->r, 4) << 32;
        n->dstrd = (uint32_t)((cap >> 32) & 0xF);
        mmio_w32(n->r, 0x14, 0);                                  /* CC.EN = 0 */
        for (int i = 0; i < 2000 && (mmio_r32(n->r, 0x1C) & 1); i++) kdelay_ms(1);
        mmio_w32(n->r, 0x24, (QN - 1) | (QN - 1) << 16);
        uint64_t asq = (uintptr_t)n->asq, acq = (uintptr_t)n->acq;
        mmio_w32(n->r, 0x28, (uint32_t)asq); mmio_w32(n->r, 0x2C, (uint32_t)(asq >> 32));
        mmio_w32(n->r, 0x30, (uint32_t)acq); mmio_w32(n->r, 0x34, (uint32_t)(acq >> 32));
        mmio_w32(n->r, 0x14, 1 | (6u << 16) | (4u << 20));        /* EN, 64-byte SQ, 16-byte CQ entries */
        int ok = 0;
        for (int i = 0; i < 5000; i++) { if (mmio_r32(n->r, 0x1C) & 1) { ok = 1; break; } kdelay_ms(1); }
        if (!ok) continue;
        n->aphase = n->iophase = 1;
        uint32_t cmd[16] = { 0 };
        uint64_t idp = (uintptr_t)idbuf;
        cmd[0] = 0x06; cmd[6] = (uint32_t)idp; cmd[7] = (uint32_t)(idp >> 32); cmd[10] = 1;   /* identify controller */
        if (submit(n, 0, cmd) != 0) continue;
        struct disk *d = rs_kmalloc(sizeof *d);
        if (!d) return found;
        memset(d, 0, sizeof *d);
        memcpy(d->model, idbuf + 24, 40);
        d->model[40] = 0;
        for (int j = 39; j >= 0 && (d->model[j] == ' ' || !d->model[j]); j--) d->model[j] = 0;
        /* I/O completion + submission queues */
        uint64_t cq = (uintptr_t)n->iocq, sq = (uintptr_t)n->iosq;
        memset(cmd, 0, sizeof cmd);
        cmd[0] = 0x05; cmd[6] = (uint32_t)cq; cmd[7] = (uint32_t)(cq >> 32); cmd[10] = (QN - 1) << 16 | 1; cmd[11] = 1;
        if (submit(n, 0, cmd) != 0) continue;
        memset(cmd, 0, sizeof cmd);
        cmd[0] = 0x01; cmd[6] = (uint32_t)sq; cmd[7] = (uint32_t)(sq >> 32); cmd[10] = (QN - 1) << 16 | 1; cmd[11] = 1u << 16 | 1;
        if (submit(n, 0, cmd) != 0) continue;
        /* namespace 1 */
        n->nsid = 1;
        memset(cmd, 0, sizeof cmd);
        cmd[0] = 0x06; cmd[1] = 1; cmd[6] = (uint32_t)idp; cmd[7] = (uint32_t)(idp >> 32); cmd[10] = 0;
        if (submit(n, 0, cmd) != 0) continue;
        uint64_t nsze; memcpy(&nsze, idbuf, 8);
        uint8_t flbas = idbuf[26] & 0xF;
        uint32_t lbaf; memcpy(&lbaf, idbuf + 128 + 4 * flbas, 4);
        if (((lbaf >> 16) & 0xFF) != 9) { kprintf("nvme: %s uses %u-byte sectors; only 512 is supported\n", d->model, 1u << ((lbaf >> 16) & 0xFF)); continue; }
        ksnprintf(d->name, sizeof d->name, "nvme%d", seq++);
        strlcpy(d->bus, "NVMe", sizeof d->bus);
        d->sectors = nsze;
        d->read = n_read; d->write = n_write; d->priv = n;
        disk_add(d);
        found++;
    }
    return found;
}
