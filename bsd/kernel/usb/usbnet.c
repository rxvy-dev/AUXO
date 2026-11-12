/* USB network: phone tethering and USB Ethernet adapters.
 *
 * Two kinds: CDC-ECM (the standard USB Ethernet class: many adapters, some phones) and RNDIS
 * (Microsoft's, which is what Android's "USB tethering" speaks). A laptop without a supported
 * Wi-Fi card gets online through a phone this way. A receive thread does blocking bulk reads
 * into a small queue the network stack polls; sending is a bulk write. */
#include "kern.h"
#include "arch.h"
#include "rs.h"
#include "net.h"
#include "usbdev.h"

#define RXQ 32
#define RXBUF 16384

struct unet {
    struct usbd *d;
    int rndis, ctl_iface, data_iface;
    uint8_t in, out;
    volatile int gone;
    uint32_t reqid;
    uint8_t *rx, *tx, *msg;              /* DMA buffers: receive, send, RNDIS control */
    struct { uint16_t len; uint8_t data[1518]; } q[RXQ];
    volatile int qh, qt;
    struct nic nic;
};

/* ── RNDIS control messages ─────────────────────────────────────────── */
static int rndis_cmd(struct unet *u, uint32_t type, const void *body, uint32_t blen, uint32_t want)
{
    uint8_t *m = u->msg;
    uint32_t len = 12 + blen, id = ++u->reqid;
    memcpy(m, &type, 4); memcpy(m + 4, &len, 4); memcpy(m + 8, &id, 4);
    if (blen) memcpy(m + 12, body, blen);
    if (usbd_control(u->d, 0x21, 0x00, 0, (uint16_t)u->ctl_iface, m, (uint16_t)len) < 0) return -1;
    for (int tries = 0; tries < 50; tries++) {                  /* the answer: poll for it */
        int r = usbd_control(u->d, 0xA1, 0x01, 0, (uint16_t)u->ctl_iface, m, 1024);
        if (r >= 12) {
            uint32_t rt, rid;
            memcpy(&rt, m, 4); memcpy(&rid, m + 8, 4);
            if (rt == want && rid == id) return r;
            if (rt == 0x80000007 || rt == 0x00000008) continue;  /* keepalive / indicate: not ours */
        }
        rs_sleep(AUXO_HZ / 50 + 1);
    }
    return -1;
}

static int rndis_query(struct unet *u, uint32_t oid, void *out, uint32_t cap)
{
    uint8_t body[16] = { 0 };
    memcpy(body, &oid, 4);                                        /* Oid, InformationBufferLength 0, Offset 0, DeviceVcHandle 0 */
    int r = rndis_cmd(u, 0x00000004, body, 16, 0x80000004);
    if (r < 24) return -1;
    uint32_t status, blen, boff;
    memcpy(&status, u->msg + 12, 4); memcpy(&blen, u->msg + 16, 4); memcpy(&boff, u->msg + 20, 4);
    if (status || 8 + boff + blen > (uint32_t)r) return -1;
    if (blen > cap) blen = cap;
    memcpy(out, u->msg + 8 + boff, blen);
    return (int)blen;
}

static int rndis_set(struct unet *u, uint32_t oid, const void *val, uint32_t vlen)
{
    uint8_t body[64] = { 0 };
    uint32_t off = 20;                                            /* from the RequestId field */
    memcpy(body, &oid, 4); memcpy(body + 4, &vlen, 4); memcpy(body + 8, &off, 4);
    memcpy(body + 16, val, vlen);
    int r = rndis_cmd(u, 0x00000005, body, 16 + vlen, 0x80000005);
    uint32_t status = 1;
    if (r >= 16) memcpy(&status, u->msg + 12, 4);
    return status ? -1 : 0;
}

/* ── frames ─────────────────────────────────────────────────────────── */
static void enqueue(struct unet *u, const uint8_t *f, uint32_t len)
{
    if (len < 14 || len > 1518) return;
    int n = (u->qh + 1) % RXQ;
    if (n == u->qt) return;                                      /* full: drop */
    memcpy(u->q[u->qh].data, f, len);
    u->q[u->qh].len = (uint16_t)len;
    u->qh = n;
}

static void rx_thread(uintptr_t arg)
{
    struct unet *u = (struct unet *)arg;
    while (!u->gone) {
        int r = usbd_xfer(u->d, u->in, u->rx, RXBUF, 1000);
        if (r == -4 || u->gone) break;
        if (r == -2) { usbd_clear_halt(u->d, u->in); continue; }
        if (r <= 0) continue;
        if (!u->rndis) { enqueue(u, u->rx, (uint32_t)r); continue; }
        for (int o = 0; o + 44 <= r; ) {                         /* RNDIS: one or more packet messages */
            uint32_t type, mlen, doff, dlen;
            memcpy(&type, u->rx + o, 4); memcpy(&mlen, u->rx + o + 4, 4);
            memcpy(&doff, u->rx + o + 8, 4); memcpy(&dlen, u->rx + o + 12, 4);
            if (type != 1 || mlen < 44 || o + (int)mlen > r) break;
            if (8 + doff + dlen <= mlen) enqueue(u, u->rx + o + 8 + doff, dlen);
            o += (int)mlen;
        }
    }
}

static int n_recv(struct nic *n, void *buf, size_t cap)
{
    struct unet *u = n->priv;
    if (u->qt == u->qh) return 0;
    size_t len = u->q[u->qt].len;
    if (len > cap) len = cap;
    memcpy(buf, u->q[u->qt].data, len);
    u->qt = (u->qt + 1) % RXQ;
    return (int)len;
}

static int n_send(struct nic *n, const void *frame, size_t len)
{
    struct unet *u = n->priv;
    if (u->gone || len > 1518) return -1;
    uint32_t total = (uint32_t)len;
    if (u->rndis) {
        memset(u->tx, 0, 44);
        uint32_t type = 1, mlen = 44 + (uint32_t)len, doff = 36, dlen = (uint32_t)len;
        memcpy(u->tx, &type, 4); memcpy(u->tx + 4, &mlen, 4); memcpy(u->tx + 8, &doff, 4); memcpy(u->tx + 12, &dlen, 4);
        memcpy(u->tx + 44, frame, len);
        total = mlen;
    } else memcpy(u->tx, frame, len);
    int r = usbd_xfer(u->d, u->out, u->tx, total, 2000);
    if (r == -2) usbd_clear_halt(u->d, u->out);
    return r == (int)total ? 0 : -1;
}

static int n_link(struct nic *n) { return !((struct unet *)n->priv)->gone; }

static void detach(void *p)
{
    struct unet *u = p;
    u->gone = 1;
    kprintf("usb : network %s unplugged\n", u->nic.model);
}

static int hexval(int c) { return c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1; }

int usbnet_probe(struct usbd *d, const uint8_t *cfg, int len)
{
    int ctl = -1, data = -1, rndis = 0, data_alt = 0, imac = 0, cur_if = -1, cur_alt = 0, cur_data = 0;
    int in = 0, out = 0, mps_in = 0, mps_out = 0;
    for (int o = 0; o + 2 <= len && cfg[o] >= 2; o += cfg[o]) {
        int type = cfg[o + 1];
        if (type == 4 && o + 9 <= len) {
            cur_if = cfg[o + 2]; cur_alt = cfg[o + 3];
            int c = cfg[o + 5], sc = cfg[o + 6], pr = cfg[o + 7];
            cur_data = 0;
            if (ctl < 0 && ((c == 0xE0 && sc == 1 && pr == 3) || (c == 2 && sc == 2 && pr == 0xFF) || (c == 0xEF && sc == 4 && pr == 1))) { ctl = cur_if; rndis = 1; }
            else if (ctl < 0 && c == 2 && sc == 6) { ctl = cur_if; rndis = 0; }               /* CDC-ECM */
            else if (c == 0x0A && ctl >= 0) { cur_data = 1; }
        } else if (type == 0x24 && o + 4 <= len && cur_if == ctl && cfg[o + 2] == 0x0F) imac = cfg[o + 3];   /* Ethernet functional */
        else if (type == 5 && cur_data && o + 7 <= len && (cfg[o + 3] & 3) == 2) {
            if (data < 0 || data == cur_if) {
                data = cur_if; data_alt = cur_alt;
                int mps = cfg[o + 4] | (cfg[o + 5] & 7) << 8;
                if (cfg[o + 2] & 0x80) { in = cfg[o + 2]; mps_in = mps; } else { out = cfg[o + 2]; mps_out = mps; }
            }
        }
    }
    if (ctl < 0 || data < 0 || !in || !out) return 0;
    struct unet *u = rs_kmalloc(sizeof *u);
    uint64_t pg = pmm_alloc_contig(RXBUF / PAGE_SIZE + 2);
    if (!u || !pg) return 0;
    memset(u, 0, sizeof *u);
    u->d = d; u->rndis = rndis; u->ctl_iface = ctl; u->data_iface = data; u->in = (uint8_t)in; u->out = (uint8_t)out;
    u->rx = (uint8_t *)(uintptr_t)pg; u->tx = u->rx + RXBUF; u->msg = u->tx + PAGE_SIZE;
    if (data_alt) usbd_control(d, 0x01, 11, (uint16_t)data_alt, (uint16_t)data, 0, 0);       /* SET_INTERFACE */
    struct usb_ep eps[2] = { { (uint8_t)in, 2, (uint16_t)mps_in, 0, 0 }, { (uint8_t)out, 2, (uint16_t)mps_out, 0, 0 } };
    if (usbd_configure(d, eps, 2) < 0) { kprintf("usb : %s: couldn't open its endpoints\n", usbd_name(d)); return 0; }
    usbd_set_driver(d, u, detach, "network");
    int have_mac = 0;
    if (rndis) {
        uint8_t init[12] = { 1, 0, 0, 0, 0, 0, 0, 0, 0, 0x40, 0, 0 };   /* version 1.0, max transfer 16 KiB */
        if (rndis_cmd(u, 0x00000002, init, 12, 0x80000002) < 0) { kprintf("usb : %s: RNDIS didn't start\n", usbd_name(d)); return 1; }
        if (rndis_query(u, 0x01010101, u->nic.mac, 6) == 6) have_mac = 1;      /* OID_802_3_PERMANENT_ADDRESS */
        uint32_t filter = 0x0000000D;                                           /* directed, multicast, broadcast */
        rndis_set(u, 0x0001010E, &filter, 4);                                   /* OID_GEN_CURRENT_PACKET_FILTER */
    } else {
        char s[32] = "";
        uint8_t sd[64];
        if (imac && usbd_control(d, 0x80, 6, (uint16_t)(0x0300 | imac), 0x0409, sd, sizeof sd) >= 26) {
            for (int i = 0; i < 12; i++) s[i] = (char)sd[2 + 2 * i];
            have_mac = 1;
            for (int i = 0; i < 6; i++) {
                int hi = hexval(s[2 * i]), lo = hexval(s[2 * i + 1]);
                if (hi < 0 || lo < 0) { have_mac = 0; break; }
                u->nic.mac[i] = (uint8_t)(hi << 4 | lo);
            }
        }
        usbd_control(d, 0x21, 0x43, 0x0E, (uint16_t)ctl, 0, 0);               /* SET_ETHERNET_PACKET_FILTER */
    }
    if (!have_mac) {                                                            /* make one up (locally administered) */
        uint64_t t = rdtsc();
        u->nic.mac[0] = 0x02;
        for (int i = 1; i < 6; i++) u->nic.mac[i] = (uint8_t)(t >> (8 * i));
    }
    u->nic.driver = rndis ? "rndis" : "cdc-ecm";
    ksnprintf(u->nic.model, sizeof u->nic.model, "%s", usbd_name(d));
    u->nic.send = n_send; u->nic.recv = n_recv; u->nic.link = n_link; u->nic.priv = u;
    rs_spawn("usbnet", rx_thread, (uintptr_t)u);
    kprintf("usb : network: %s (%s)\n", u->nic.model, rndis ? "RNDIS, phone tethering" : "USB Ethernet");
    net_attach(&u->nic);
    return 1;
}
