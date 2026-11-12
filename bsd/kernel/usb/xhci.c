/* USB through xHCI: keyboards and mice on any modern PC.
 *
 * Most machines from the last ten years have no PS/2 port, and a UEFI boot often leaves no
 * firmware emulation of one either, so without this a PC has no keyboard. One kernel thread
 * polls the controller (no interrupts): it takes the controller from the BIOS, resets it,
 * enumerates devices on the root ports and behind USB 2 hubs, and runs HID boot-protocol
 * keyboards and mice, including combo receivers that are both. Keys go through the same
 * scancode path as PS/2 (with key repeat done here, since USB keyboards don't repeat), mouse
 * reports into the same event queue. Devices can be plugged and unplugged at any time. */
#include "kern.h"
#include "arch.h"
#include "rs.h"
#include "pci.h"
#include "usb.h"

#define NTRB      256
#define MAXDEV    32
#define MAXHC     4
#define NFN       3                 /* HID functions per device (keyboard + mouse receivers) */
#define INFLIGHT  8                 /* interrupt transfers queued per endpoint */

enum { TRB_NORMAL = 1, TRB_SETUP = 2, TRB_DATA = 3, TRB_STATUS = 4, TRB_LINK = 6, TRB_ENABLE_SLOT = 9,
       TRB_DISABLE_SLOT = 10, TRB_ADDRESS = 11, TRB_CONFIG_EP = 12, TRB_EVAL = 13, TRB_RESET_EP = 14,
       TRB_SET_DEQ = 16, EV_TRANSFER = 32, EV_CMD = 33, EV_PORT = 34 };
enum { CC_SUCCESS = 1, CC_STALL = 6, CC_SHORT = 13 };
enum { SPEED_FS = 1, SPEED_LS = 2, SPEED_HS = 3, SPEED_SS = 4 };
enum { FN_NONE, FN_KBD, FN_MOUSE };

struct trb { volatile uint32_t d0, d1, d2, d3; };
struct ring { struct trb *t; int idx, cycle; };

struct fn {
    int kind, iface, dci, mps;
    struct ring r;
    uint8_t *bufs;                  /* INFLIGHT x 64 bytes */
    uint8_t prev[8];                /* keyboard: last report */
};

struct hc;
struct dev {
    int used, slot, speed, rootport, depth, port, parent;   /* port: on the parent hub (0 = root) */
    uint32_t route;
    int ttslot, ttport;
    struct hc *hc;
    uint64_t outctx;
    struct ring ep0;
    uint8_t *buf;                   /* control transfers, one page */
    volatile int ctrl_done, ctrl_cc, ctrl_len;
    int is_hub, nports;
    uint32_t hubconn;               /* hub: ports with a device attached (bitmask) */
    struct fn fn[NFN];
    uint16_t vid, pid;
    char name[48];
};

struct hc {
    struct pci_dev *pci;
    volatile uint8_t *cap, *op, *rt;
    volatile uint32_t *db;
    int maxslots, maxports, ctxsz;
    uint64_t *dcbaa;
    struct ring cmd;
    struct trb *ev;
    int evidx, evcycle;
    volatile int cmd_done, cmd_cc, cmd_slot;
    uint64_t cmd_ptr;
    int rootdev[256];               /* device index per root port, -1 none */
};

static struct hc hcs[MAXHC];
static int nhc;
static struct dev devs[MAXDEV];
static int nkbd, nmouse;

static uint32_t rd32(volatile uint8_t *b, uint32_t o) { return *(volatile uint32_t *)(b + o); }
static void wr32(volatile uint8_t *b, uint32_t o, uint32_t v) { *(volatile uint32_t *)(b + o) = v; }
static void wr64(volatile uint8_t *b, uint32_t o, uint64_t v) { wr32(b, o, (uint32_t)v); wr32(b, o + 4, (uint32_t)(v >> 32)); }

static void *page(void)
{
    uint64_t p = pmm_alloc_page();
    if (p) memset((void *)(uintptr_t)p, 0, PAGE_SIZE);
    return (void *)(uintptr_t)p;
}
static uint64_t phys(const volatile void *p) { return (uint64_t)(uintptr_t)p; }

static void msleep(int ms) { rs_sleep((uint64_t)(ms * AUXO_HZ + 999) / 1000); }

/* ── rings ───────────────────────────────────────────────────────────── */
static int ring_init(struct ring *r)
{
    r->t = page();
    if (!r->t) return -1;
    r->idx = 0; r->cycle = 1;
    struct trb *l = &r->t[NTRB - 1];
    l->d0 = (uint32_t)phys(r->t); l->d1 = (uint32_t)(phys(r->t) >> 32);
    l->d3 = TRB_LINK << 10 | 2;     /* toggle cycle */
    return 0;
}

static struct trb *ring_put(struct ring *r, uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3)
{
    struct trb *t = &r->t[r->idx];
    t->d0 = d0; t->d1 = d1; t->d2 = d2;
    mb();
    t->d3 = (d3 & ~1u) | (uint32_t)r->cycle;
    if (++r->idx == NTRB - 1) {
        struct trb *l = &r->t[NTRB - 1];
        mb();
        l->d3 = (l->d3 & ~1u) | (uint32_t)r->cycle;
        r->idx = 0;
        r->cycle ^= 1;
    }
    return t;
}

static void ring_free(struct ring *r) { if (r->t) pmm_free_page(phys(r->t)); r->t = 0; }

/* ── contexts ───────────────────────────────────────────────────────── */
static volatile uint32_t *ictx(struct hc *h, uint8_t *in, int i) { return (volatile uint32_t *)(in + (size_t)(i + 1) * (size_t)h->ctxsz); }
/* i = -1: input control context; 0: slot; n: endpoint DCI n */

/* ── events ─────────────────────────────────────────────────────────── */
static void hid_report(struct dev *d, struct fn *f, const uint8_t *r, int len);

static struct dev *dev_by_slot(struct hc *h, int slot)
{
    for (int i = 0; i < MAXDEV; i++) if (devs[i].used && devs[i].hc == h && devs[i].slot == slot) return &devs[i];
    return 0;
}

static void transfer_event(struct hc *h, struct trb *e)
{
    int slot = (int)(e->d3 >> 24), ep = (int)((e->d3 >> 16) & 31), cc = (int)(e->d2 >> 24);
    struct dev *d = dev_by_slot(h, slot);
    if (!d) return;
    if (ep == 1) { d->ctrl_cc = cc; d->ctrl_len = (int)(e->d2 & 0xFFFFFF); d->ctrl_done = 1; return; }
    for (int i = 0; i < NFN; i++) {
        struct fn *f = &d->fn[i];
        if (!f->kind || f->dci != ep) continue;
        uint64_t tp = (uint64_t)e->d0 | (uint64_t)e->d1 << 32;
        struct trb *t = (struct trb *)(uintptr_t)tp;
        uint64_t bp = (uint64_t)t->d0 | (uint64_t)t->d1 << 32;
        int want = (int)(t->d2 & 0x1FFFF), got = want - (int)(e->d2 & 0xFFFFFF);
        if (cc == CC_SUCCESS || cc == CC_SHORT) hid_report(d, f, (const uint8_t *)(uintptr_t)bp, got);
        if (cc == CC_SUCCESS || cc == CC_SHORT) {      /* queue it again */
            ring_put(&f->r, (uint32_t)bp, (uint32_t)(bp >> 32), (uint32_t)f->mps, TRB_NORMAL << 10 | 1 << 5 | 1 << 2);
            h->db[slot] = (uint32_t)f->dci;
        }
        return;
    }
}

static void poll_events(struct hc *h)
{
    int n = 0;
    for (;;) {
        struct trb *e = &h->ev[h->evidx];
        if ((e->d3 & 1) != (uint32_t)h->evcycle) break;
        mb();
        int type = (int)((e->d3 >> 10) & 63);
        if (type == EV_CMD) {
            uint64_t p = (uint64_t)e->d0 | (uint64_t)e->d1 << 32;
            if (p == h->cmd_ptr) { h->cmd_cc = (int)(e->d2 >> 24); h->cmd_slot = (int)(e->d3 >> 24); h->cmd_done = 1; }
        } else if (type == EV_TRANSFER) transfer_event(h, e);
        if (++h->evidx == NTRB) { h->evidx = 0; h->evcycle ^= 1; }
        n++;
    }
    if (n) wr64(h->rt, 0x38, phys(&h->ev[h->evidx]) | 8);      /* ERDP, clear EHB */
}

static int command(struct hc *h, uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3, int *slot)
{
    h->cmd_done = 0;
    struct trb *t = ring_put(&h->cmd, d0, d1, d2, d3);
    h->cmd_ptr = phys(t);
    h->db[0] = 0;
    for (int i = 0; i < 300; i++) {
        for (int k = 0; k < 2000 && !h->cmd_done; k++) { poll_events(h); __asm__ volatile("pause"); }
        if (h->cmd_done) break;
        msleep(5);
    }
    if (!h->cmd_done) return -1;
    if (slot) *slot = h->cmd_slot;
    return h->cmd_cc;
}

/* ── control transfers ──────────────────────────────────────────────── */
static void recover_ep0(struct dev *d)
{
    struct hc *h = d->hc;
    command(h, 0, 0, 0, TRB_RESET_EP << 10 | 1u << 16 | (uint32_t)d->slot << 24, 0);
    uint64_t deq = phys(&d->ep0.t[d->ep0.idx]) | (uint64_t)d->ep0.cycle;
    command(h, (uint32_t)deq, (uint32_t)(deq >> 32), 0, TRB_SET_DEQ << 10 | 1u << 16 | (uint32_t)d->slot << 24, 0);
}

/* returns bytes transferred, or <0 */
static int control(struct dev *d, uint8_t rt, uint8_t req, uint16_t val, uint16_t idx, uint16_t len)
{
    struct hc *h = d->hc;
    int in = rt & 0x80;
    uint32_t setup0 = rt | (uint32_t)req << 8 | (uint32_t)val << 16, setup1 = idx | (uint32_t)len << 16;
    d->ctrl_done = 0;
    ring_put(&d->ep0, setup0, setup1, 8, TRB_SETUP << 10 | 1 << 6 | (len ? (in ? 3u : 2u) << 16 : 0));
    if (len) ring_put(&d->ep0, (uint32_t)phys(d->buf), (uint32_t)(phys(d->buf) >> 32), len, TRB_DATA << 10 | (in ? 1u << 16 : 0));
    ring_put(&d->ep0, 0, 0, 0, TRB_STATUS << 10 | 1 << 5 | (len && in ? 0 : 1u << 16));
    h->db[d->slot] = 1;
    for (int i = 0; i < 200 && !d->ctrl_done; i++) {
        for (int k = 0; k < 2000 && !d->ctrl_done; k++) { poll_events(h); __asm__ volatile("pause"); }
        if (!d->ctrl_done) msleep(5);
    }
    if (!d->ctrl_done) { recover_ep0(d); return -1; }
    if (d->ctrl_cc != CC_SUCCESS && d->ctrl_cc != CC_SHORT) { recover_ep0(d); return -2; }
    /* the event for the status stage carries no length; a short data stage stops early */
    return d->ctrl_cc == CC_SHORT ? len - d->ctrl_len : len;
}

static int get_desc(struct dev *d, int type, int index, uint16_t lang, int len)
{
    return control(d, 0x80, 6, (uint16_t)(type << 8 | index), lang, (uint16_t)len);
}

static void get_string(struct dev *d, int index, char *out, size_t n)
{
    out[0] = 0;
    if (!index || get_desc(d, 3, 0, 0, 4) < 4) return;
    uint16_t lang = (uint16_t)(d->buf[2] | d->buf[3] << 8);
    int r = get_desc(d, 3, index, lang, 255);
    if (r < 2) return;
    int len = d->buf[0] < r ? d->buf[0] : r;
    size_t o = 0;
    for (int i = 2; i + 1 < len && o + 1 < n; i += 2) {
        uint16_t c = (uint16_t)(d->buf[i] | d->buf[i + 1] << 8);
        out[o++] = c >= 32 && c < 127 ? (char)c : '?';
    }
    while (o && out[o - 1] == ' ') o--;
    out[o] = 0;
}

/* ── devices ────────────────────────────────────────────────────────── */
static void detach(int di);

static int ep_interval(int speed, int binterval)
{
    if (speed == SPEED_HS || speed >= SPEED_SS) return binterval >= 1 ? binterval - 1 : 0;
    int ms = binterval ? binterval : 1, e = 3;                       /* 2^e x 125 µs */
    while (e < 10 && (1 << (e + 1)) <= ms * 8) e++;
    return e;
}

static const char *kind_name(int k) { return k == FN_KBD ? "keyboard" : k == FN_MOUSE ? "mouse" : "device"; }

static int setup_device(int di)
{
    struct dev *d = &devs[di];
    struct hc *h = d->hc;
    int slot;
    if (command(h, 0, 0, 0, TRB_ENABLE_SLOT << 10, &slot) != CC_SUCCESS || slot <= 0) return -1;
    d->slot = slot;
    d->buf = page();
    void *out = page();
    uint8_t *in = page();
    if (!d->buf || !out || !in || ring_init(&d->ep0) < 0) return -1;
    d->outctx = phys(out);
    h->dcbaa[slot] = d->outctx;

    int mps0 = d->speed == SPEED_SS ? 512 : d->speed == SPEED_HS ? 64 : 8;
    ictx(h, in, -1)[1] = 3;                                           /* add slot + EP0 */
    volatile uint32_t *sc = ictx(h, in, 0), *e0 = ictx(h, in, 1);
    sc[0] = (d->route & 0xFFFFF) | (uint32_t)d->speed << 20 | 1u << 27;
    sc[1] = (uint32_t)d->rootport << 16;
    sc[2] = (uint32_t)d->ttslot | (uint32_t)d->ttport << 8;
    e0[1] = 3u << 1 | 4u << 3 | (uint32_t)mps0 << 16;                /* CErr 3, control */
    uint64_t deq = phys(d->ep0.t) | 1;
    e0[2] = (uint32_t)deq; e0[3] = (uint32_t)(deq >> 32);
    e0[4] = 8;
    int cc = command(h, (uint32_t)phys(in), (uint32_t)(phys(in) >> 32), 0, TRB_ADDRESS << 10 | (uint32_t)slot << 24, 0);
    if (cc != CC_SUCCESS) { pmm_free_page(phys(in)); return -1; }
    msleep(2);

    if (get_desc(d, 1, 0, 0, 8) < 8) { pmm_free_page(phys(in)); return -1; }
    int real = d->buf[7];
    if (d->speed >= SPEED_SS) real = 1 << real;
    if (real && real != mps0) {                                       /* full speed: 8/16/32/64 */
        memset(in, 0, PAGE_SIZE);
        ictx(h, in, -1)[1] = 2;
        ictx(h, in, 1)[1] = 3u << 1 | 4u << 3 | (uint32_t)real << 16;
        command(h, (uint32_t)phys(in), (uint32_t)(phys(in) >> 32), 0, TRB_EVAL << 10 | (uint32_t)slot << 24, 0);
    }
    if (get_desc(d, 1, 0, 0, 18) < 18) { pmm_free_page(phys(in)); return -1; }
    uint8_t dclass = d->buf[4];
    d->vid = (uint16_t)(d->buf[8] | d->buf[9] << 8);
    d->pid = (uint16_t)(d->buf[10] | d->buf[11] << 8);
    int iprod = d->buf[15], imfr = d->buf[14];

    /* the configuration: find HID boot interfaces or a hub */
    if (get_desc(d, 2, 0, 0, 9) < 9) { pmm_free_page(phys(in)); return -1; }
    int total = d->buf[2] | d->buf[3] << 8;
    if (total > 1024) total = 1024;
    if (get_desc(d, 2, 0, 0, total) < 9) { pmm_free_page(phys(in)); return -1; }
    uint8_t cfg[1024];
    memcpy(cfg, d->buf, (size_t)total);
    int cfgval = cfg[5], cur_iface = -1, cur_kind = 0, hub_ep = 0, hub_mps = 0, hub_int = 0, nf = 0;
    d->is_hub = dclass == 9;
    for (int o = 0; o + 2 <= total && cfg[o] >= 2; o += cfg[o]) {
        int type = cfg[o + 1];
        if (type == 4 && o + 9 <= total) {                            /* interface */
            cur_iface = cfg[o + 2];
            cur_kind = 0;
            if (cfg[o + 5] == 9) d->is_hub = 1;
            if (cfg[o + 5] == 3 && cfg[o + 6] == 1 && cfg[o + 3] == 0)  /* HID, boot subclass, alt 0 */
                cur_kind = cfg[o + 7] == 1 ? FN_KBD : cfg[o + 7] == 2 ? FN_MOUSE : 0;
        } else if (type == 5 && o + 7 <= total && (cfg[o + 2] & 0x80) && (cfg[o + 3] & 3) == 3) {
            int epn = cfg[o + 2] & 15, mps = (cfg[o + 4] | cfg[o + 5] << 8) & 0x7FF;
            if (d->is_hub && !hub_ep) { hub_ep = epn; hub_mps = mps; hub_int = cfg[o + 6]; }
            if (cur_kind && nf < NFN) {
                struct fn *f = &d->fn[nf++];
                f->kind = cur_kind; f->iface = cur_iface; f->dci = epn * 2 + 1;
                f->mps = mps > 64 ? 64 : mps;
                f->prev[0] = (uint8_t)cfg[o + 6];                     /* bInterval, until configured */
                cur_kind = 0;                                         /* one endpoint per interface */
            }
        }
    }
    if (d->speed != SPEED_LS) {
        get_string(d, iprod, d->name, sizeof d->name);
        if (!d->name[0]) get_string(d, imfr, d->name, sizeof d->name);
    }
    if (!d->name[0]) ksnprintf(d->name, sizeof d->name, "%04x:%04x", d->vid, d->pid);
    if (control(d, 0x00, 9, (uint16_t)cfgval, 0, 0) < 0) { pmm_free_page(phys(in)); return -1; }   /* SET_CONFIGURATION */

    if (d->is_hub) {
        if (control(d, 0xA0, 6, 0x2900, 0, 9) < 3) { pmm_free_page(phys(in)); return -1; }   /* hub descriptor */
        d->nports = d->buf[2] > 15 ? 15 : d->buf[2];
        int ttt = (d->buf[3] >> 5) & 3, pwr = d->buf[5] * 2;
        memset(in, 0, PAGE_SIZE);
        int dci = hub_ep ? hub_ep * 2 + 1 : 1;
        ictx(h, in, -1)[1] = 1 | (hub_ep ? 1u << dci : 0);
        volatile uint32_t *s = ictx(h, in, 0);
        s[0] = (d->route & 0xFFFFF) | (uint32_t)d->speed << 20 | 1u << 26 | (uint32_t)dci << 27;
        s[1] = (uint32_t)d->rootport << 16 | (uint32_t)d->nports << 24;
        s[2] = (uint32_t)d->ttslot | (uint32_t)d->ttport << 8 | (uint32_t)ttt << 16;
        if (hub_ep) {
            struct ring tmp;
            if (ring_init(&tmp) == 0) {                               /* never used: we poll port status */
                volatile uint32_t *e = ictx(h, in, dci);
                e[0] = (uint32_t)ep_interval(d->speed, hub_int) << 16;
                e[1] = 3u << 1 | 7u << 3 | (uint32_t)(hub_mps ? hub_mps : 1) << 16;
                uint64_t q = phys(tmp.t) | 1;
                e[2] = (uint32_t)q; e[3] = (uint32_t)(q >> 32);
                e[4] = (uint32_t)(hub_mps ? hub_mps : 1);
            }
        }
        command(h, (uint32_t)phys(in), (uint32_t)(phys(in) >> 32), 0, TRB_CONFIG_EP << 10 | (uint32_t)slot << 24, 0);
        pmm_free_page(phys(in));
        for (int p = 1; p <= d->nports; p++) control(d, 0x23, 3, 8, (uint16_t)p, 0);   /* PORT_POWER */
        msleep(pwr > 100 ? pwr : 100);
        kprintf("usb : hub with %d ports (%s)\n", d->nports, d->name);
        return 0;
    }

    if (!nf) { pmm_free_page(phys(in)); return 0; }                   /* something we don't drive */
    memset(in, 0, PAGE_SIZE);
    int maxdci = 1;
    uint32_t add = 1;
    for (int i = 0; i < nf; i++) {
        struct fn *f = &d->fn[i];
        if (ring_init(&f->r) < 0 || !(f->bufs = page())) { f->kind = 0; continue; }
        add |= 1u << f->dci;
        if (f->dci > maxdci) maxdci = f->dci;
        volatile uint32_t *e = ictx(h, in, f->dci);
        e[0] = (uint32_t)ep_interval(d->speed, f->prev[0]) << 16;
        e[1] = 3u << 1 | 7u << 3 | (uint32_t)f->mps << 16;            /* interrupt IN */
        uint64_t q = phys(f->r.t) | 1;
        e[2] = (uint32_t)q; e[3] = (uint32_t)(q >> 32);
        e[4] = (uint32_t)f->mps | (uint32_t)f->mps << 16;
        f->prev[0] = 0;
    }
    ictx(h, in, -1)[1] = add;
    volatile uint32_t *s = ictx(h, in, 0);
    s[0] = (d->route & 0xFFFFF) | (uint32_t)d->speed << 20 | (uint32_t)maxdci << 27;
    s[1] = (uint32_t)d->rootport << 16;
    s[2] = (uint32_t)d->ttslot | (uint32_t)d->ttport << 8;
    cc = command(h, (uint32_t)phys(in), (uint32_t)(phys(in) >> 32), 0, TRB_CONFIG_EP << 10 | (uint32_t)slot << 24, 0);
    pmm_free_page(phys(in));
    if (cc != CC_SUCCESS) return -1;
    for (int i = 0; i < nf; i++) {
        struct fn *f = &d->fn[i];
        if (!f->kind) continue;
        control(d, 0x21, 0x0B, 0, (uint16_t)f->iface, 0);              /* SET_PROTOCOL(boot) */
        control(d, 0x21, 0x0A, 0, (uint16_t)f->iface, 0);              /* SET_IDLE(0): only on change */
        for (int k = 0; k < INFLIGHT; k++) {
            uint64_t bp = phys(f->bufs) + (uint64_t)k * 64;
            ring_put(&f->r, (uint32_t)bp, (uint32_t)(bp >> 32), (uint32_t)f->mps, TRB_NORMAL << 10 | 1 << 5 | 1 << 2);
        }
        h->db[slot] = (uint32_t)f->dci;
        if (f->kind == FN_KBD) nkbd++; else nmouse++;
        if (f->kind == FN_MOUSE) mouse_usb_attached();
        kprintf("usb : %s: %s\n", kind_name(f->kind), d->name);
    }
    return 0;
}

static int new_dev(struct hc *h, int rootport, int parent, int port, int speed)
{
    int di = -1;
    for (int i = 0; i < MAXDEV; i++) if (!devs[i].used) { di = i; break; }
    if (di < 0) return -1;
    struct dev *d = &devs[di];
    memset(d, 0, sizeof *d);
    d->used = 1; d->hc = h; d->speed = speed; d->rootport = rootport; d->parent = parent; d->port = port;
    if (parent >= 0) {
        struct dev *p = &devs[parent];
        d->depth = p->depth + 1;
        d->route = p->route | (uint32_t)(port > 15 ? 15 : port) << (4 * (d->depth - 1));
        if ((speed == SPEED_LS || speed == SPEED_FS) && p->speed == SPEED_HS) { d->ttslot = p->slot; d->ttport = port; }
        else { d->ttslot = p->ttslot; d->ttport = p->ttport; }
    }
    if (setup_device(di) < 0) {
        kprintf("usb : a device on port %d didn't answer\n", port ? port : rootport);
        detach(di);
        return -1;
    }
    return di;
}

static void detach(int di)
{
    struct dev *d = &devs[di];
    if (!d->used) return;
    for (int i = 0; i < MAXDEV; i++) if (devs[i].used && devs[i].parent == di && i != di) detach(i);
    struct hc *h = d->hc;
    for (int i = 0; i < NFN; i++) {
        struct fn *f = &d->fn[i];
        if (f->kind == FN_KBD) nkbd--;
        if (f->kind == FN_MOUSE) nmouse--;
        if (f->kind) kprintf("usb : %s unplugged\n", kind_name(f->kind));
        ring_free(&f->r);
        if (f->bufs) pmm_free_page(phys(f->bufs));
    }
    if (d->slot) {
        command(h, 0, 0, 0, TRB_DISABLE_SLOT << 10 | (uint32_t)d->slot << 24, 0);
        h->dcbaa[d->slot] = 0;
    }
    ring_free(&d->ep0);
    if (d->buf) pmm_free_page(phys(d->buf));
    if (d->outctx) pmm_free_page(d->outctx);
    d->used = 0;
}

/* ── ports ──────────────────────────────────────────────────────────── */
#define PORTSC(h, p) (0x400 + 0x10 * ((p) - 1))
#define PS_KEEP 0x0E00C3E0u          /* RW bits we must write back unchanged (no RW1C, not PED) */

static int root_reset(struct hc *h, int p)
{
    uint32_t v = rd32(h->op, PORTSC(h, p));
    if (v & 2) return (int)((v >> 10) & 15);                         /* USB 3: already enabled */
    wr32(h->op, PORTSC(h, p), (v & PS_KEEP) | 1u << 4);              /* PR */
    for (int i = 0; i < 50; i++) {
        msleep(10);
        v = rd32(h->op, PORTSC(h, p));
        if (!(v & (1u << 4)) && (v & (1u << 21))) break;
    }
    wr32(h->op, PORTSC(h, p), (v & PS_KEEP) | 0x00FE0000u);          /* clear change bits */
    msleep(20);                                                      /* recovery time */
    v = rd32(h->op, PORTSC(h, p));
    if (!(v & 2)) return -1;
    return (int)((v >> 10) & 15);
}

static void scan_root(struct hc *h)
{
    for (int p = 1; p <= h->maxports; p++) {
        uint32_t v = rd32(h->op, PORTSC(h, p));
        int conn = v & 1;
        if (v & 0x00FE0000u) wr32(h->op, PORTSC(h, p), (v & PS_KEEP) | (v & 0x00FE0000u));
        if (!conn && h->rootdev[p] >= 0) { detach(h->rootdev[p]); h->rootdev[p] = -1; continue; }
        if (!conn || h->rootdev[p] >= 0 || h->rootdev[p] == -2) continue;
        msleep(100);                                                 /* debounce */
        int speed = root_reset(h, p);
        if (speed <= 0) { h->rootdev[p] = -2; continue; }            /* retried after unplug */
        int di = new_dev(h, p, -1, 0, speed);
        h->rootdev[p] = di >= 0 ? di : -2;
    }
    for (int p = 1; p <= h->maxports; p++)                           /* failed ports: retry when unplugged */
        if (h->rootdev[p] == -2 && !(rd32(h->op, PORTSC(h, p)) & 1)) h->rootdev[p] = -1;
}

static void scan_hub(int hi)
{
    struct dev *hub = &devs[hi];
    for (int p = 1; p <= hub->nports; p++) {
        if (control(hub, 0xA3, 0, 0, (uint16_t)p, 4) < 4) continue;
        uint32_t st = (uint32_t)(hub->buf[0] | hub->buf[1] << 8 | hub->buf[2] << 16 | hub->buf[3] << 24);
        if (st & (1u << 16)) control(hub, 0x23, 1, 16, (uint16_t)p, 0);   /* clear C_PORT_CONNECTION */
        int conn = st & 1, have = (hub->hubconn >> p) & 1;
        if (!conn && have) {
            for (int i = 0; i < MAXDEV; i++)
                if (devs[i].used && devs[i].parent == hi && devs[i].port == p) detach(i);
            hub->hubconn &= ~(1u << p);
        }
        if (!conn || have) continue;
        msleep(100);
        control(hub, 0x23, 3, 4, (uint16_t)p, 0);                    /* PORT_RESET */
        int ok = 0;
        for (int i = 0; i < 50; i++) {
            msleep(10);
            if (control(hub, 0xA3, 0, 0, (uint16_t)p, 4) < 4) break;
            st = (uint32_t)(hub->buf[0] | hub->buf[1] << 8 | hub->buf[2] << 16 | hub->buf[3] << 24);
            if (st & (1u << 20)) { ok = 1; break; }
        }
        control(hub, 0x23, 1, 20, (uint16_t)p, 0);                   /* clear C_PORT_RESET */
        hub->hubconn |= 1u << p;                                     /* even on failure: no retry loop */
        if (!ok || !(st & 2)) continue;
        msleep(20);
        int speed = (st & (1u << 9)) ? SPEED_LS : (st & (1u << 10)) ? SPEED_HS : SPEED_FS;
        if (hub->speed >= SPEED_SS) speed = SPEED_SS;
        new_dev(hub->hc, hub->rootport, hi, p, speed);
    }
}

/* ── HID ────────────────────────────────────────────────────────────── */
/* HID usage → PS/2 scancode set 1; 0x80 bit = E0-prefixed */
static const uint8_t hid2sc[0x66] = {
    [0x04] = 0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32, 0x31, 0x18, 0x19,
    0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C,
    [0x1E] = 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
    [0x28] = 0x1C, 0x01, 0x0E, 0x0F, 0x39, 0x0C, 0x0D, 0x1A, 0x1B, 0x2B, 0x2B, 0x27, 0x28, 0x29, 0x33, 0x34, 0x35, 0x3A,
    [0x3A] = 0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x57, 0x58,
    [0x46] = 0xB7, 0x46, 0,
    [0x49] = 0xD2, 0xC7, 0xC9, 0xD3, 0xCF, 0xD1, 0xCD, 0xCB, 0xD0, 0xC8,
    [0x53] = 0x45, 0xB5, 0x37, 0x4A, 0x4E, 0x9C, 0x4F, 0x50, 0x51, 0x4B, 0x4C, 0x4D, 0x47, 0x48, 0x49, 0x52, 0x53,
    [0x64] = 0x56, 0xDD,
};
static const uint8_t mod2sc[8] = { 0x1D, 0x2A, 0x38, 0xDB, 0x9D, 0x36, 0xB8, 0xDC };

static void send_sc(uint8_t sc, int up)
{
    if (!sc) return;
    if (sc & 0x80) input_scancode(0xE0);
    input_scancode((uint8_t)((sc & 0x7F) | (up ? 0x80 : 0)));
}

static uint8_t rep_usage;
static uint64_t rep_at;

static void hid_report(struct dev *d, struct fn *f, const uint8_t *r, int len)
{
    (void)d;
    if (f->kind == FN_MOUSE) {
        if (len < 3) return;
        int wheel = len >= 4 ? -(int8_t)r[3] : 0;
        mouse_push((int8_t)r[1], (int8_t)r[2], wheel, r[0] & 7);
        return;
    }
    if (len < 8) return;
    if (r[2] == 1) return;                                           /* rollover error: keep state */
    for (int b = 0; b < 8; b++) {                                    /* modifiers */
        int was = (f->prev[0] >> b) & 1, now = (r[0] >> b) & 1;
        if (was != now) send_sc(mod2sc[b], !now);
    }
    for (int i = 2; i < 8; i++) {                                    /* released */
        uint8_t u = f->prev[i];
        if (!u) continue;
        int still = 0;
        for (int j = 2; j < 8; j++) if (r[j] == u) still = 1;
        if (!still) { if (u < sizeof hid2sc) send_sc(hid2sc[u], 1); if (u == rep_usage) rep_usage = 0; }
    }
    for (int i = 2; i < 8; i++) {                                    /* pressed */
        uint8_t u = r[i];
        if (!u) continue;
        int was = 0;
        for (int j = 2; j < 8; j++) if (f->prev[j] == u) was = 1;
        if (!was && u < sizeof hid2sc) {
            send_sc(hid2sc[u], 0);
            rep_usage = u; rep_at = kernel_ticks() + AUXO_HZ / 2;   /* repeat after 500 ms */
        }
    }
    memcpy(f->prev, r, 8);
}

static void key_repeat(void)
{
    if (!rep_usage || kernel_ticks() < rep_at) return;
    send_sc(hid2sc[rep_usage], 0);
    rep_at = kernel_ticks() + (AUXO_HZ >= 30 ? AUXO_HZ / 30 : 1);   /* ~30 per second */
}

/* ── controllers ────────────────────────────────────────────────────── */
static void bios_handoff(struct hc *h)
{
    uint32_t hcc = rd32(h->cap, 0x10);
    uint32_t off = (hcc >> 16) << 2;
    for (int n = 0; off && n < 64; n++) {
        volatile uint8_t *x = h->cap + off;
        uint32_t v = rd32(x, 0);
        if ((v & 0xFF) == 1) {                                       /* USB legacy support */
            wr32(x, 0, v | 1u << 24);                                /* OS owned */
            for (int i = 0; i < 100 && (rd32(x, 0) & (1u << 16)); i++) kdelay_ms(10);
            if (rd32(x, 0) & (1u << 16)) wr32(x, 0, rd32(x, 0) & ~(1u << 16));   /* BIOS ignored us */
            wr32(x, 4, rd32(x, 4) & 0x1F1EE);                         /* SMIs off, keep RW1C clear */
        }
        uint32_t next = (v >> 8) & 0xFF;
        if (!next) break;
        off += next << 2;
    }
}

static int hc_init(struct hc *h, struct pci_dev *pd)
{
    h->pci = pd;
    pci_enable(pd, 1);
    if (pd->vendor == 0x8086) {
        /* Intel 7/8/9-series chipsets start with USB ports routed to EHCI: switch them over */
        static const uint16_t ids[] = { 0x1E31, 0x8C31, 0x9C31, 0x8CB1, 0x9CB1 };
        for (unsigned i = 0; i < sizeof ids / sizeof ids[0]; i++)
            if (pd->device == ids[i]) {
                pci_write32(pd, 0xD8, pci_read32(pd, 0xDC));
                pci_write32(pd, 0xD0, pci_read32(pd, 0xD4));
            }
    }
    uint64_t len = pd->barsize[0] ? pd->barsize[0] : 0x10000;
    h->cap = vm_map_mmio(pd->bar[0], len, 1);
    if (!h->cap) return -1;
    h->op = h->cap + (rd32(h->cap, 0) & 0xFF);
    h->rt = h->cap + (rd32(h->cap, 0x18) & ~0x1Fu);
    h->db = (volatile uint32_t *)(h->cap + (rd32(h->cap, 0x14) & ~3u));
    uint32_t hcs1 = rd32(h->cap, 0x04), hcs2 = rd32(h->cap, 0x08), hcc = rd32(h->cap, 0x10);
    h->maxslots = (int)(hcs1 & 0xFF);
    h->maxports = (int)(hcs1 >> 24);
    if (h->maxports > 255) h->maxports = 255;
    h->ctxsz = (hcc & 4) ? 64 : 32;
    if (h->maxslots > MAXDEV) h->maxslots = MAXDEV;
    bios_handoff(h);

    wr32(h->op, 0, rd32(h->op, 0) & ~1u);                           /* stop */
    for (int i = 0; i < 100 && !(rd32(h->op, 4) & 1); i++) kdelay_ms(1);
    wr32(h->op, 0, 2);                                               /* HCRST */
    for (int i = 0; i < 1000 && (rd32(h->op, 0) & 2); i++) kdelay_ms(1);
    for (int i = 0; i < 1000 && (rd32(h->op, 4) & (1u << 11)); i++) kdelay_ms(1);   /* CNR */
    if (rd32(h->op, 0) & 2) return -1;

    wr32(h->op, 0x38, (uint32_t)h->maxslots);                        /* CONFIG: slots enabled */
    h->dcbaa = page();
    if (!h->dcbaa) return -1;
    int nsp = (int)(((hcs2 >> 21) & 0x1F) << 5 | ((hcs2 >> 27) & 0x1F));
    if (nsp) {                                                       /* scratchpad buffers */
        uint64_t *arr = page();
        if (!arr || nsp > 512) return -1;
        for (int i = 0; i < nsp; i++) { void *p = page(); if (!p) return -1; arr[i] = phys(p); }
        h->dcbaa[0] = phys(arr);
    }
    wr64(h->op, 0x30, phys(h->dcbaa));
    if (ring_init(&h->cmd) < 0) return -1;
    wr64(h->op, 0x18, phys(h->cmd.t) | 1);                           /* CRCR */

    h->ev = page();
    uint64_t *erst = page();
    if (!h->ev || !erst) return -1;
    erst[0] = phys(h->ev); erst[1] = NTRB;
    h->evidx = 0; h->evcycle = 1;
    volatile uint8_t *ir = h->rt + 0x20;                             /* interrupter 0 */
    wr32(ir, 0x08, 1);                                               /* ERSTSZ */
    wr64(ir, 0x18, phys(h->ev));                                     /* ERDP */
    wr64(ir, 0x10, phys(erst));                                      /* ERSTBA */
    wr32(ir, 0x00, 0);                                               /* no interrupts: polled */
    if (hcc & 8)                                                     /* port power control */
        for (int p = 1; p <= h->maxports; p++) {
            uint32_t v = rd32(h->op, PORTSC(h, p));
            if (!(v & (1u << 9))) wr32(h->op, PORTSC(h, p), (v & PS_KEEP) | 1u << 9);
        }
    wr32(h->op, 0, 1);                                               /* run */
    for (int i = 0; i < 100 && (rd32(h->op, 4) & 1); i++) kdelay_ms(1);
    for (int p = 0; p < 256; p++) h->rootdev[p] = -1;
    return 0;
}

static volatile int settled;

static void usb_thread(uintptr_t arg)
{
    (void)arg;
    msleep(50);                                                      /* let ports settle after reset */
    uint64_t next_scan = 0;
    for (;;) {
        for (int i = 0; i < nhc; i++) poll_events(&hcs[i]);
        key_repeat();
        if (kernel_ticks() >= next_scan) {
            for (int i = 0; i < nhc; i++) scan_root(&hcs[i]);
            for (int i = 0; i < MAXDEV; i++) if (devs[i].used && devs[i].is_hub) scan_hub(i);
            next_scan = kernel_ticks() + AUXO_HZ / 2;
            settled = 1;
        }
        wait_tick();
    }
}

int usb_init(void)
{
    for (int i = 0; i < pci_count() && nhc < MAXHC; i++) {
        struct pci_dev *pd = pci_get(i);
        if (pd->class != 0x0C || pd->subclass != 0x03 || pd->prog_if != 0x30) continue;
        struct hc *h = &hcs[nhc];
        memset(h, 0, sizeof *h);
        if (hc_init(h, pd) < 0) { kprintf("usb : xHCI at %02x:%02x.%d didn't start\n", pd->bus, pd->dev, pd->fn); continue; }
        pd->driver = "xhci";
        nhc++;
    }
    if (nhc) rs_spawn("usb", usb_thread, 0);
    return nhc;
}

int usb_controllers(void) { return nhc; }
int usb_keyboards(void) { return nkbd; }
int usb_mice(void) { return nmouse; }

int usb_list(char *out, size_t n)
{
    size_t o = 0;
    int c = 0;
    for (int i = 0; i < MAXDEV; i++) {
        struct dev *d = &devs[i];
        if (!d->used) continue;
        const char *what = d->is_hub ? "hub" : 0;
        char kinds[32] = "";
        for (int k = 0; k < NFN; k++)
            if (d->fn[k].kind) { if (kinds[0]) strlcpy(kinds + strlen(kinds), "+", sizeof kinds - strlen(kinds)); strlcpy(kinds + strlen(kinds), kind_name(d->fn[k].kind), sizeof kinds - strlen(kinds)); }
        if (!what) what = kinds[0] ? kinds : "(no driver)";
        static const char *const sp[] = { "?", "12M", "1.5M", "480M", "5G", "10G" };
        char path[16];
        if (d->depth) ksnprintf(path, sizeof path, "%d-%d.%d", (int)(d->hc - hcs) + 1, d->rootport, d->port);
        else ksnprintf(path, sizeof path, "%d-%d", (int)(d->hc - hcs) + 1, d->rootport);
        int r = ksnprintf(out + o, n - o, "%-7s %04x:%04x  %-5s %-16s %s\n", path, d->vid, d->pid,
                          sp[d->speed < 6 ? d->speed : 0], what, d->name);
        if (r < 0 || (size_t)r >= n - o) break;
        o += (size_t)r;
        c++;
    }
    return c;
}

void usb_wait_settled(int ms)
{
    if (!nhc) return;
    uint64_t end = kernel_ticks() + (uint64_t)ms * AUXO_HZ / 1000;
    while (!settled && kernel_ticks() < end) rs_sleep(1);      /* from a thread (the shell) */
}
