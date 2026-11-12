/* Intel High Definition Audio: the sound chip of nearly every PC since 2004 (and QEMU's
 * intel-hda). The controller moves samples between memory and the codecs over the HDA link;
 * the codec (Realtek, Conexant, IDT, the QEMU one…) turns them into sound.
 *
 * What this does: reset the controller, talk to the codecs through the CORB/RIRB rings,
 * walk each codec's audio function group for an output pin (speaker, headphones, line out)
 * and a path to it from a DAC, power the path up, unmute it and turn it up; then play one
 * output stream from a ring buffer in memory (one buffer-descriptor entry per period).
 * Where the stream is comes from the link position register, polled. kern/snd.c puts the
 * Linux (ALSA) interface on top. */
#include "kern.h"
#include "arch.h"
#include "rs.h"
#include "pci.h"
#include "hda.h"

/* controller registers */
#define GCAP     0x00
#define GCTL     0x08
#define STATESTS 0x0E
#define INTCTL   0x20
#define CORBLBASE 0x40
#define CORBUBASE 0x44
#define CORBWP   0x48
#define CORBRP   0x4A
#define CORBCTL  0x4C
#define CORBSIZE 0x4E
#define RIRBLBASE 0x50
#define RIRBUBASE 0x54
#define RIRBWP   0x58
#define RINTCNT  0x5A
#define RIRBCTL  0x5C
#define RIRBSTS  0x5D
#define RIRBSIZE 0x5E
#define DPLBASE  0x70
#define DPUBASE  0x74
/* stream descriptor registers, from the descriptor's base */
#define SD_CTL   0x00          /* 3 bytes: run, reset, stream tag */
#define SD_STS   0x03
#define SD_LPIB  0x04
#define SD_CBL   0x08
#define SD_LVI   0x0C
#define SD_FMT   0x12
#define SD_BDPL  0x18
#define SD_BDPU  0x1C

/* codec verbs */
#define V_GET_PARAM   0xF00
#define V_GET_CONN    0xF02
#define V_SET_SEL     0x701
#define V_SET_POWER   0x705
#define V_SET_STREAM  0x706
#define V_SET_PINCTL  0x707
#define V_SET_EAPD    0x70C
#define V_GET_CONFIG  0xF1C
#define V_SET_FORMAT  0x2      /* 4-bit verbs, 16-bit payload */
#define V_SET_AMP     0x3
/* parameters */
#define P_NODES   0x04
#define P_FGTYPE  0x05
#define P_WCAP    0x09
#define P_PINCAP  0x0C
#define P_CONNLEN 0x0E
#define P_OUTAMP  0x12

static struct {
    volatile uint8_t *m;
    struct pci_dev *pci;
    uint32_t *corb; uint64_t *rirb;
    uint16_t corb_n, rirb_n, rirb_rp;
    int iss, oss, sd;          /* the output stream descriptor we use (register base) */
    int cad, dac, pin, path[8], npath;
    int amp_max, vol;          /* the DAC's output amplifier: steps, and the volume set (0..100) */
    int muted;
    char codec[48], pin_kind[24];
    uint8_t *ring; uint64_t ring_phys; uint32_t ring_cap;
    uint64_t *bdl;
    int running;
    uint32_t cbl;
    int dead;                  /* a command went unanswered: stop asking (a codec or controller we don't follow) */
    int budget;                /* commands left for setting up: a strange codec can't hold up the boot */
    uint8_t seen[128];         /* nodes the path search has been through */
} h;

int hda_present(void) { return h.m && h.dac > 0; }
const char *hda_codec_name(void) { return h.codec; }
const char *hda_output_name(void) { return h.pin_kind; }

static int hda_debug = 0;
static uint32_t r32(uint32_t o) { return mmio_r32(h.m, o); }
static void w32(uint32_t o, uint32_t v) { mmio_w32(h.m, o, v); }
static uint16_t r16(uint32_t o) { return mmio_r16(h.m, o); }
static void w16(uint32_t o, uint16_t v) { mmio_w16(h.m, o, v); }
static uint8_t r8(uint32_t o) { return mmio_r8(h.m, o); }
static void w8(uint32_t o, uint8_t v) { mmio_w8(h.m, o, v); }

static int wait_bits(uint32_t off, int size, uint32_t mask, uint32_t want, int ms)
{
    for (int i = 0; i <= ms; i++) {
        uint32_t v = size == 1 ? r8(off) : size == 2 ? r16(off) : r32(off);
        if ((v & mask) == want) return 0;
        kdelay_ms(1);
    }
    return -1;
}

/* one command, one response (polled) */
static int cmd(int nid, uint32_t verb, uint32_t payload, uint32_t *resp)
{
    if (h.dead || h.budget-- <= 0) return -1;
    uint32_t c = (uint32_t)h.cad << 28 | (uint32_t)(nid & 0x7F) << 20;
    c |= verb <= 0xF ? (verb << 16 | (payload & 0xFFFF)) : (verb << 8 | (payload & 0xFF));
    uint16_t wp = (uint16_t)((r16(CORBWP) + 1) % h.corb_n);
    h.corb[wp] = c;
    w16(CORBWP, wp);
    uint64_t until = mono_ns() + 50000000ULL;                 /* 50 ms: codecs answer in microseconds */
    for (;;) {
        uint16_t rwp = r16(RIRBWP) & 0xFF;
        if (rwp != h.rirb_rp) {
            h.rirb_rp = (uint16_t)((h.rirb_rp + 1) % h.rirb_n);
            uint64_t e = h.rirb[h.rirb_rp];
            w8(RIRBSTS, 0x5);
            if (resp) *resp = (uint32_t)e;
            return 0;
        }
        if (mono_ns() > until) break;
        __asm__ volatile("pause");
    }
    h.dead = 1;
    if (hda_debug) kprintf("hda: no answer to %08x (corb wp %u rp %u, rirb wp %u, ours %u, sts %x)\n", c, r16(CORBWP), r16(CORBRP), r16(RIRBWP), h.rirb_rp, r8(RIRBSTS));
    return -1;
}

static uint32_t param(int nid, int p) { uint32_t v = 0; cmd(nid, V_GET_PARAM, (uint32_t)p, &v); return v; }

static int connections(int nid, int *out, int max)
{
    uint32_t len = param(nid, P_CONNLEN);
    int n = (int)(len & 0x7F), lng = (len >> 7) & 1, k = 0;
    for (int i = 0; i < n && k < max; i += lng ? 2 : 4) {
        uint32_t v = 0;
        cmd(nid, V_GET_CONN, (uint32_t)i, &v);
        for (int j = 0; j < (lng ? 2 : 4) && i + j < n && k < max; j++)
            out[k++] = (int)(lng ? (v >> (16 * j)) & 0x7FFF : (v >> (8 * j)) & 0x7F);
    }
    return k;
}

static int wtype(int nid) { return (int)((param(nid, P_WCAP) >> 20) & 0xF); }

/* depth-first from a pin to an audio output (DAC), through mixers and selectors */
static int find_dac(int nid, int depth)
{
    if (depth > 5 || nid < 0 || nid > 127 || h.seen[nid]) return 0;
    h.seen[nid] = 1;                                         /* each node once: no going round mixers forever */
    int t = wtype(nid);
    if (t == 0) { h.path[h.npath++] = nid; return 1; }
    if (depth && t != 2 && t != 3) return 0;              /* only mixers and selectors in between */
    int c[32], n = connections(nid, c, 32);
    for (int i = 0; i < n; i++) {
        int save = h.npath;
        h.path[h.npath++] = nid;
        if (h.npath < 8 && find_dac(c[i], depth + 1)) {
            if (t == 3 || t == 4) cmd(nid, V_SET_SEL, (uint32_t)i, 0);   /* selectors and pins: this input */
            return 1;
        }
        h.npath = save;
    }
    return 0;
}

/* a widget's amplifiers unmuted at 0 dB (the "offset" step of its amp capabilities) */
static void unmute(int nid)
{
    uint32_t oc = param(nid, P_OUTAMP), ic = param(nid, 0x0D);
    cmd(nid, V_SET_AMP, 0xB000 | (oc & 0x7F), 0);            /* output, left + right */
    for (int i = 0; i < 8; i++) cmd(nid, V_SET_AMP, 0x7000 | (uint32_t)i << 8 | (ic & 0x7F), 0);   /* inputs */
}

void hda_set_volume(int pct, int mute)
{
    if (!hda_present()) return;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    h.vol = pct; h.muted = mute;
    int g = h.amp_max * pct / 100;
    cmd(h.dac, V_SET_AMP, 0xB000 | (mute || !pct ? 0x80 : 0) | (uint32_t)g, 0);
}
int hda_volume(void) { return h.vol; }
int hda_muted(void) { return h.muted; }

static int setup_codec(int cad)
{
    h.cad = cad;
    uint32_t vid = param(0, 0x00);
    uint32_t sub = param(0, P_NODES);
    if (hda_debug) kprintf("hda: codec %d vendor %x nodes %x\n", cad, vid, sub);
    int start = (int)((sub >> 16) & 0xFF), count = (int)(sub & 0xFF);
    for (int fg = start; fg < start + count; fg++) {
        if (hda_debug) kprintf("hda: codec %d fg %d type %x (nodes %x)\n", cad, fg, param(fg, P_FGTYPE), param(fg, P_NODES));
        if ((param(fg, P_FGTYPE) & 0xFF) != 1) continue;    /* the audio function group */
        cmd(fg, V_SET_POWER, 0, 0);
        uint32_t ws = param(fg, P_NODES);
        int ws0 = (int)((ws >> 16) & 0xFF), wn = (int)(ws & 0xFF);
        /* the best output pin: speaker, then headphones, then line out; connected ones first */
        int best = -1, best_score = -1;
        for (int nid = ws0; nid < ws0 + wn; nid++) {
            if (hda_debug) { uint32_t cf = 0; cmd(nid, V_GET_CONFIG, 0, &cf); kprintf("hda: nid %d wcap %x pincap %x cfg %x conn %x\n", nid, param(nid, P_WCAP), param(nid, P_PINCAP), cf, param(nid, P_CONNLEN)); }
            if (wtype(nid) != 4) continue;
            uint32_t pc = param(nid, P_PINCAP);
            if (!(pc & (1u << 4))) continue;                  /* can't output */
            uint32_t cfg = 0;
            cmd(nid, V_GET_CONFIG, 0, &cfg);
            int conn = (int)(cfg >> 30), dev = (int)((cfg >> 20) & 0xF);
            if (conn == 1) continue;                          /* nothing attached here */
            int score = dev == 1 ? 30 : dev == 2 ? 20 : dev == 0 ? 10 : 1;
            if (conn == 2) score += 5;                        /* built in */
            if (score > best_score) { best_score = score; best = nid; }
        }
        if (best < 0) continue;
        h.npath = 0;
        memset(h.seen, 0, sizeof h.seen);
        if (!find_dac(best, 0)) continue;
        h.pin = best;
        h.dac = h.path[h.npath - 1];
        uint32_t cfg = 0;
        cmd(best, V_GET_CONFIG, 0, &cfg);
        int dev = (int)((cfg >> 20) & 0xF);
        strlcpy(h.pin_kind, dev == 1 ? "speaker" : dev == 2 ? "headphones" : dev == 0 ? "line out" : "output", sizeof h.pin_kind);
        for (int i = 0; i < h.npath; i++) { cmd(h.path[i], V_SET_POWER, 0, 0); unmute(h.path[i]); }
        uint32_t pc = param(best, P_PINCAP);
        cmd(best, V_SET_PINCTL, 0x40 | (dev == 2 ? 0x80 : 0), 0);
        if (pc & (1u << 16)) cmd(best, V_SET_EAPD, 0x02, 0);   /* the external amplifier on */
        uint32_t ac = param(h.dac, P_OUTAMP);
        if (!ac) ac = param(fg, P_OUTAMP);
        h.amp_max = (int)(ac >> 8) & 0x7F;
        if (!h.amp_max) h.amp_max = (int)(ac & 0x7F);
        h.vol = 80;
        hda_set_volume(h.vol, 0);
        ksnprintf(h.codec, sizeof h.codec, "%04x:%04x", (unsigned)(vid >> 16), (unsigned)(vid & 0xFFFF));
        return 0;
    }
    return -1;
}

int hda_init(void)
{
    struct pci_dev *d = pci_find_class(0x04, 0x03, 0);
    if (!d || d->bario[0] || !d->bar[0]) return -1;
    h.m = vm_map_mmio(d->bar[0], d->barsize[0] ? d->barsize[0] : 0x4000, 1);
    if (!h.m) return -1;
    h.pci = d;
    pci_enable(d, 1);
    /* reset */
    w32(GCTL, r32(GCTL) & ~1u);
    if (wait_bits(GCTL, 4, 1, 0, 100) < 0) { h.m = 0; return -1; }
    kdelay_ms(1);
    w32(GCTL, r32(GCTL) | 1u);
    if (wait_bits(GCTL, 4, 1, 1, 100) < 0) { h.m = 0; return -1; }
    kdelay_ms(2);                                             /* codecs announce themselves */
    uint16_t gcap = r16(GCAP);
    h.iss = (gcap >> 8) & 0xF; h.oss = (gcap >> 12) & 0xF;
    if (!h.oss) { h.m = 0; return -1; }
    h.sd = 0x80 + h.iss * 0x20;
    w32(INTCTL, 0);
    /* CORB and RIRB: 256 entries each, in one page each */
    uint64_t cp = pmm_alloc_page(), rp = pmm_alloc_page();
    if (!cp || !rp) { h.m = 0; return -1; }
    h.corb = (uint32_t *)(uintptr_t)cp; h.rirb = (uint64_t *)(uintptr_t)rp;
    memset(h.corb, 0, 4096); memset(h.rirb, 0, 4096);
    w8(CORBCTL, 0); w8(RIRBCTL, 0);
    wait_bits(CORBCTL, 1, 2, 0, 50); wait_bits(RIRBCTL, 1, 2, 0, 50);
    uint8_t cs = r8(CORBSIZE), rs = r8(RIRBSIZE);
    int csel = (cs & 0x40) ? 2 : (cs & 0x20) ? 1 : 0, rsel = (rs & 0x40) ? 2 : (rs & 0x20) ? 1 : 0;
    w8(CORBSIZE, (uint8_t)((cs & ~3) | csel)); w8(RIRBSIZE, (uint8_t)((rs & ~3) | rsel));
    h.corb_n = csel == 2 ? 256 : csel == 1 ? 16 : 2; h.rirb_n = rsel == 2 ? 256 : rsel == 1 ? 16 : 2;
    w32(CORBLBASE, (uint32_t)cp); w32(CORBUBASE, (uint32_t)(cp >> 32));
    w32(RIRBLBASE, (uint32_t)rp); w32(RIRBUBASE, (uint32_t)(rp >> 32));
    w16(CORBRP, 0x8000);                                      /* read pointer reset */
    wait_bits(CORBRP, 2, 0x8000, 0x8000, 10);
    w16(CORBRP, 0);
    wait_bits(CORBRP, 2, 0x8000, 0, 10);
    w16(CORBWP, 0);
    w16(RIRBWP, 0x8000);
    w16(RINTCNT, 0xFF);
    h.rirb_rp = 0;
    w8(CORBCTL, 2); w8(RIRBCTL, 2);                           /* DMA on (no interrupts: polled) */
    uint16_t codecs = r16(STATESTS);
    w16(STATESTS, codecs);
    int ok = -1;
    for (int c = 0; c < 15 && ok < 0; c++) if (codecs & (1u << c)) { h.dead = 0; h.budget = 3000; ok = setup_codec(c); }
    h.budget = 1 << 30;
    if (ok < 0) { kprintf("hda : controller %04x:%04x, but no codec with an output\n", d->vendor, d->device); h.dac = 0; return -1; }
    /* the play-out ring: 256 KiB, and the buffer descriptor list */
    h.ring_cap = 256 * 1024;
    h.ring_phys = pmm_alloc_contig(h.ring_cap / PAGE_SIZE);
    uint64_t bp = pmm_alloc_page();
    if (!h.ring_phys || !bp) { h.dac = 0; return -1; }
    h.ring = (uint8_t *)(uintptr_t)h.ring_phys;
    h.bdl = (uint64_t *)(uintptr_t)bp;
    memset(h.ring, 0, h.ring_cap);
    return 0;
}

uint8_t *hda_ring(uint32_t *cap) { *cap = h.ring_cap; return h.ring; }

/* rate 44100 or 48000, 16-bit, 2 channels; the ring's first bytes bytes, in periods */
int hda_start(int rate, int channels, uint32_t bytes, uint32_t period)
{
    if (!hda_present() || !bytes || !period || bytes > h.ring_cap || bytes % period) return -1;
    hda_stop();
    int sd = h.sd;
    w8(sd + SD_CTL, 1);                                       /* stream reset */
    wait_bits(sd + SD_CTL, 1, 1, 1, 10);
    w8(sd + SD_CTL, 0);
    wait_bits(sd + SD_CTL, 1, 1, 0, 10);
    int n = (int)(bytes / period);
    if (n > 256) return -1;
    for (int i = 0; i < n; i++) {
        uint64_t a = h.ring_phys + (uint64_t)i * period;
        h.bdl[i * 2] = a;
        h.bdl[i * 2 + 1] = period;                           /* length; no interrupt on completion */
    }
    uint64_t bp = (uint64_t)(uintptr_t)h.bdl;
    w32(sd + SD_BDPL, (uint32_t)bp); w32(sd + SD_BDPU, (uint32_t)(bp >> 32));
    w32(sd + SD_CBL, bytes);
    w16(sd + SD_LVI, (uint16_t)(n - 1));
    uint16_t fmt = (uint16_t)((rate == 44100 ? 1u << 14 : 0) | (1u << 4) | (uint32_t)(channels - 1));
    w16(sd + SD_FMT, fmt);
    cmd(h.dac, V_SET_FORMAT, fmt, 0);
    cmd(h.dac, V_SET_STREAM, 1u << 4, 0);                      /* stream tag 1, channel 0 */
    w8(sd + SD_CTL + 2, (uint8_t)(1u << 4));                   /* tag 1 (bits 23:20) */
    w8(sd + SD_STS, 0x1C);
    h.cbl = bytes;
    w8(sd + SD_CTL, 2);                                       /* run */
    h.running = 1;
    return 0;
}

void hda_stop(void)
{
    if (!h.m || !h.running) return;
    w8(h.sd + SD_CTL, 0);
    wait_bits(h.sd + SD_CTL, 1, 2, 0, 10);
    h.running = 0;
}

/* where the controller is reading, in bytes into the ring (0 .. bytes-1) */
uint32_t hda_position(void)
{
    if (!h.running) return 0;
    uint32_t p = r32(h.sd + SD_LPIB);
    return h.cbl ? p % h.cbl : 0;
}
