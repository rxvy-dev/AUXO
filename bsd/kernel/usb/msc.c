/* USB storage: sticks, card readers and external drives (mass storage class, bulk-only
 * transport, SCSI commands). Each one becomes a disk (usb0, usb1 …) that can be read, written,
 * and installed to; unplugging it makes the disk go away. */
#include "kern.h"
#include "arch.h"
#include "rs.h"
#include "disk.h"
#include "usbdev.h"

struct msc {
    struct usbd *d;
    uint8_t in, out, lun;
    uint32_t tag, bsize;
    volatile int gone, lock;
    uint8_t *page;                  /* CBW, CSW and small replies */
    struct disk disk;
};

static int nmsc;

static void mlock(struct msc *m) { while (__atomic_exchange_n(&m->lock, 1, __ATOMIC_ACQUIRE)) rs_yield(); }
static void munlock(struct msc *m) { __atomic_store_n(&m->lock, 0, __ATOMIC_RELEASE); }

static void reset_recovery(struct msc *m)
{
    usbd_control(m->d, 0x21, 0xFF, 0, 0, 0, 0);                 /* Bulk-Only Mass Storage Reset */
    usbd_clear_halt(m->d, m->in);
    usbd_clear_halt(m->d, m->out);
}

/* one SCSI command: CBW out, data, CSW in. 0 = good, 1 = check condition, <0 = transport error */
static int bot(struct msc *m, const uint8_t *cb, int cblen, int in, void *data, uint32_t len, int timeout_ms)
{
    if (m->gone) return -4;
    uint8_t *cbw = m->page;
    memset(cbw, 0, 31);
    uint32_t tag = ++m->tag;
    memcpy(cbw, "USBC", 4);
    memcpy(cbw + 4, &tag, 4);
    memcpy(cbw + 8, &len, 4);
    cbw[12] = in ? 0x80 : 0;
    cbw[13] = m->lun;
    cbw[14] = (uint8_t)cblen;
    memcpy(cbw + 15, cb, (size_t)cblen);
    int r = usbd_xfer(m->d, m->out, cbw, 31, 5000);
    if (r == -2) { usbd_clear_halt(m->d, m->out); return -3; }
    if (r != 31) { if (r != -4) reset_recovery(m); return r == -4 ? -4 : -3; }
    if (len) {
        r = usbd_xfer(m->d, in ? m->in : m->out, data, len, timeout_ms);
        if (r == -2) usbd_clear_halt(m->d, in ? m->in : m->out);   /* then read the status anyway */
        else if (r < 0) { if (r != -4) reset_recovery(m); return r == -4 ? -4 : -3; }
    }
    uint8_t *csw = m->page + 64;
    for (int tries = 0; tries < 2; tries++) {
        r = usbd_xfer(m->d, m->in, csw, 13, 5000);
        if (r == -2) { usbd_clear_halt(m->d, m->in); continue; }
        break;
    }
    if (r != 13 || memcmp(csw, "USBS", 4) || memcmp(csw + 4, &tag, 4)) { if (r != -4) reset_recovery(m); return r == -4 ? -4 : -3; }
    if (csw[12] == 2) { reset_recovery(m); return -3; }          /* phase error */
    return csw[12] ? 1 : 0;
}

static void request_sense(struct msc *m)
{
    uint8_t cb[6] = { 0x03, 0, 0, 0, 18, 0 };
    bot(m, cb, 6, 1, m->page + 128, 18, 2000);
}

static int rw(struct disk *dk, uint64_t lba, uint32_t count, void *buf, int write)
{
    struct msc *m = dk->priv;
    uint8_t *b = buf;
    mlock(m);
    while (count) {
        if (m->gone) { munlock(m); return -1; }
        uint32_t n = count > 128 ? 128 : count;                  /* 64 KiB a command */
        uint8_t cb[16];
        int cl;
        memset(cb, 0, sizeof cb);
        if (lba + n <= 0xFFFFFFFFULL) {                           /* READ(10) / WRITE(10) */
            cb[0] = write ? 0x2A : 0x28;
            cb[2] = (uint8_t)(lba >> 24); cb[3] = (uint8_t)(lba >> 16); cb[4] = (uint8_t)(lba >> 8); cb[5] = (uint8_t)lba;
            cb[7] = (uint8_t)(n >> 8); cb[8] = (uint8_t)n;
            cl = 10;
        } else {                                                  /* READ(16) / WRITE(16): past 2 TiB */
            cb[0] = write ? 0x8A : 0x88;
            for (int i = 0; i < 8; i++) cb[2 + i] = (uint8_t)(lba >> (56 - 8 * i));
            cb[10] = (uint8_t)(n >> 24); cb[11] = (uint8_t)(n >> 16); cb[12] = (uint8_t)(n >> 8); cb[13] = (uint8_t)n;
            cl = 16;
        }
        int r = -1;
        for (int tries = 0; tries < 3; tries++) {
            r = bot(m, cb, cl, !write, b, n * 512, 20000);
            if (r == 0 || r == -4) break;
            if (r == 1) request_sense(m);
        }
        if (r) { munlock(m); return -5; }
        b += (size_t)n * 512; lba += n; count -= n;
    }
    if (write) { uint8_t cb[10] = { 0x35 }; bot(m, cb, 10, 0, 0, 0, 20000); }   /* SYNCHRONIZE CACHE */
    munlock(m);
    return 0;
}

static int m_read(struct disk *d, uint64_t lba, uint32_t count, void *buf) { return rw(d, lba, count, buf, 0); }
static int m_write(struct disk *d, uint64_t lba, uint32_t count, const void *buf) { return rw(d, lba, count, (void *)buf, 1); }

static void m_detach(void *p)
{
    struct msc *m = p;
    m->gone = 1;
    m->disk.gone = 1;
    kprintf("usb : %s (%s) unplugged\n", m->disk.name, m->disk.model);
}

int msc_probe(struct usbd *d, const uint8_t *cfg, int len)
{
    int iface = -1, in = 0, out = 0, mps_in = 0, mps_out = 0, burst_in = 0, burst_out = 0, cur = 0, last = 0;
    for (int o = 0; o + 2 <= len && cfg[o] >= 2; o += cfg[o]) {
        int type = cfg[o + 1];
        if (type == 4 && o + 9 <= len) {
            cur = cfg[o + 5] == 8 && (cfg[o + 6] == 6 || cfg[o + 6] == 5 || cfg[o + 6] == 2) && cfg[o + 7] == 0x50 && iface < 0;
            if (cur) iface = cfg[o + 2];
            last = 0;
        } else if (type == 5 && cur && o + 7 <= len && (cfg[o + 3] & 3) == 2) {
            int mps = cfg[o + 4] | (cfg[o + 5] & 7) << 8;
            if (cfg[o + 2] & 0x80) { in = cfg[o + 2]; mps_in = mps; last = 1; }
            else { out = cfg[o + 2]; mps_out = mps; last = 2; }
        } else if (type == 0x30 && cur && o + 3 <= len) {         /* SuperSpeed companion: burst */
            if (last == 1) burst_in = cfg[o + 2];
            if (last == 2) burst_out = cfg[o + 2];
        }
    }
    if (iface < 0 || !in || !out) return 0;
    struct msc *m = rs_kmalloc(sizeof *m);
    uint64_t pg = pmm_alloc_page();
    if (!m || !pg) return 0;
    memset(m, 0, sizeof *m);
    m->d = d; m->in = (uint8_t)in; m->out = (uint8_t)out;
    m->page = (uint8_t *)(uintptr_t)pg;
    struct usb_ep eps[2] = { { (uint8_t)in, 2, (uint16_t)mps_in, 0, (uint8_t)burst_in },
                             { (uint8_t)out, 2, (uint16_t)mps_out, 0, (uint8_t)burst_out } };
    if (usbd_configure(d, eps, 2) < 0) { kprintf("usb : %s: couldn't open its endpoints\n", usbd_name(d)); return 0; }
    usbd_set_driver(d, m, m_detach, "storage");
    uint8_t maxlun = 0;
    if (usbd_control(d, 0xA1, 0xFE, 0, (uint16_t)iface, &maxlun, 1) < 1) maxlun = 0;   /* stall: one LUN */
    /* INQUIRY, then wait for it to be ready (card readers, drives spinning up) */
    uint8_t inq[6] = { 0x12, 0, 0, 0, 36, 0 };
    char vendor[9] = "", product[17] = "";
    if (bot(m, inq, 6, 1, m->page + 128, 36, 5000) == 0) {
        memcpy(vendor, m->page + 128 + 8, 8); memcpy(product, m->page + 128 + 16, 16);
        for (int i = 7; i >= 0 && vendor[i] == ' '; i--) vendor[i] = 0;
        for (int i = 15; i >= 0 && product[i] == ' '; i--) product[i] = 0;
    }
    int ready = 0;
    for (int i = 0; i < 40 && !ready; i++) {
        uint8_t tur[6] = { 0 };
        int r = bot(m, tur, 6, 0, 0, 0, 2000);
        if (r == 0) ready = 1;
        else { if (r == 1) request_sense(m); rs_sleep(AUXO_HZ / 8); }
        if (r == -4) return 1;
    }
    uint8_t rc[10] = { 0x25 };
    uint8_t *cap = m->page + 128;
    uint64_t blocks = 0;
    for (int tries = 0; tries < 3 && !blocks; tries++) {
        if (bot(m, rc, 10, 1, cap, 8, 5000) != 0) { request_sense(m); continue; }
        uint32_t lastlba = (uint32_t)cap[0] << 24 | (uint32_t)cap[1] << 16 | (uint32_t)cap[2] << 8 | cap[3];
        m->bsize = (uint32_t)cap[4] << 24 | (uint32_t)cap[5] << 16 | (uint32_t)cap[6] << 8 | cap[7];
        if (lastlba == 0xFFFFFFFFu) {                             /* over 2 TiB: READ CAPACITY(16) */
            uint8_t rc16[16] = { 0x9E, 0x10 };
            rc16[13] = 32;
            if (bot(m, rc16, 16, 1, cap, 32, 5000) != 0) continue;
            uint64_t l = 0;
            for (int k = 0; k < 8; k++) l = l << 8 | cap[k];
            m->bsize = (uint32_t)cap[8] << 24 | (uint32_t)cap[9] << 16 | (uint32_t)cap[10] << 8 | cap[11];
            blocks = l + 1;
        } else blocks = (uint64_t)lastlba + 1;
    }
    struct disk *dk = &m->disk;
    ksnprintf(dk->name, sizeof dk->name, "usb%d", nmsc++);
    ksnprintf(dk->model, sizeof dk->model, "%s%s%s", vendor, vendor[0] && product[0] ? " " : "", product[0] ? product : usbd_name(d));
    strlcpy(dk->bus, "USB", sizeof dk->bus);
    dk->removable = 1;
    if (!blocks) { kprintf("usb : %s (%s): no medium (an empty card reader?)\n", dk->name, dk->model); return 1; }
    if (m->bsize != 512) { kprintf("usb : %s (%s) has %u-byte sectors: only 512 is supported\n", dk->name, dk->model, m->bsize); return 1; }
    dk->sectors = blocks;
    dk->read = m_read; dk->write = m_write; dk->priv = m;
    disk_add(dk);
    kprintf("usb : storage %s: %s, %lu MB\n", dk->name, dk->model, (unsigned long)(blocks / 2048));
    (void)maxlun;
    return 1;
}
