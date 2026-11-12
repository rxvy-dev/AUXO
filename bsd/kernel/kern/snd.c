/* Sound for Linux programs: ALSA's kernel interface, the one alsa-lib (and through it aplay,
 * SDL, GStreamer, mpv, PulseAudio …) talks to, on top of kern/hda.c.
 *
 *   /dev/snd/controlC0   the card: its name, its PCM devices, and two mixer controls
 *                        ("Master Playback Volume" 0–100, "Master Playback Switch")
 *   /dev/snd/pcmC0D0p    playback: 16-bit stereo at 44100 or 48000 Hz, read/write access
 *                        (no mmap: alsa-lib's plug layer converts anything else, and keeps
 *                        its pointers in step with SNDRV_PCM_IOCTL_SYNC_PTR)
 *
 * The parameter space works as ALSA's does (pcm_lib.c's interval arithmetic and rules), since
 * alsa-lib asks the kernel to narrow it down rather than doing it itself. The card's driver
 * name is "AuxoHDA", so alsa-lib's "default" device is plughw (no dmix, which wants SysV IPC).
 * One program plays at a time, as with ALSA's hw devices. */
#include "kern.h"
#include "arch.h"
#include "rs.h"
#include "fs.h"
#include "uproc.h"
#include "hda.h"
#include "snd.h"

#define EPERM_ 1
#define EINTR_ 4
#define EIO_ 5
#define ENXIO_ 6
#define EAGAIN_ 11
#define EBUSY_ 16
#define EFAULT_ 14
#define EINVAL_ 22
#define ENOTTY_ 25
#define EPIPE_ 32
#define ENOENT_ 2
#define EBADFD_ 77
#define ESTRPIPE_ 86
#define ENOSYS_SND 38

/* ── the parameter space (struct snd_pcm_hw_params) ─────────────────── */
enum { P_ACCESS, P_FORMAT, P_SUBFORMAT, P_SAMPLE_BITS = 8, P_FRAME_BITS, P_CHANNELS, P_RATE, P_PERIOD_TIME, P_PERIOD_SIZE,
       P_PERIOD_BYTES, P_PERIODS, P_BUFFER_TIME, P_BUFFER_SIZE, P_BUFFER_BYTES, P_TICK_TIME, P_LAST = P_TICK_TIME };
struct ivl { uint32_t min, max; uint32_t openmin : 1, openmax : 1, integer : 1, empty : 1; };
struct hwp {
    uint32_t flags;
    uint32_t masks[3][8];
    uint32_t mres[5][8];
    struct ivl iv[12];
    struct ivl ires[9];
    uint32_t rmask, cmask, info, msbits, rate_num, rate_den;
    uint64_t fifo_size;
    uint8_t reserved[64];
};
_Static_assert(sizeof(struct hwp) == 608, "snd_pcm_hw_params is 608 bytes");
#define IV(p, k) (&(p)->iv[(k) - P_SAMPLE_BITS])

#define FORMAT_S16_LE 2
#define ACCESS_RW_INTERLEAVED 3
#define RATE_A 44100
#define RATE_B 48000
#define PERIOD_BYTES_MIN 1024          /* (the position is polled every 10 ms: no tiny periods) */
#define PERIOD_BYTES_MAX 65536
#define PERIODS_MIN 2
#define PERIODS_MAX 32
#define BUFFER_BYTES_MAX (256 * 1024)
#define INFO (0x1 | 0x2 | 0x00000100 | 0x00010000)   /* MMAP, MMAP_VALID, INTERLEAVED, BLOCK_TRANSFER */

static uint32_t div32(uint32_t a, uint32_t b, uint32_t *r) { if (!b) { *r = 0; return 0xFFFFFFFFu; } *r = a % b; return a / b; }
static uint32_t mul32(uint32_t a, uint32_t b) { uint64_t v = (uint64_t)a * b; return v > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)v; }
static uint32_t muldiv32(uint32_t a, uint32_t b, uint32_t c, uint32_t *r)
{
    uint64_t n = (uint64_t)a * b;
    if (!c) { *r = 0; return 0xFFFFFFFFu; }
    uint64_t q = n / c;
    *r = (uint32_t)(n % c);
    if (q > 0xFFFFFFFFu) { *r = 0; return 0xFFFFFFFFu; }
    return (uint32_t)q;
}

static int ivl_empty(const struct ivl *i) { return i->empty || i->min > i->max || (i->min == i->max && (i->openmin || i->openmax)); }
static int ivl_single(const struct ivl *i) { return !ivl_empty(i) && (i->min == i->max || (i->min + 1 == i->max && (i->openmin || i->openmax))); }
static uint32_t ivl_value(const struct ivl *i) { return i->openmin && !i->integer ? i->max : i->min; }

/* snd_interval_refine */
static int refine(struct ivl *i, const struct ivl *v)
{
    if (ivl_empty(i)) return -EINVAL_;
    int changed = 0;
    if (i->min < v->min) { i->min = v->min; i->openmin = v->openmin; changed = 1; }
    else if (i->min == v->min && !i->openmin && v->openmin) { i->openmin = 1; changed = 1; }
    if (i->max > v->max) { i->max = v->max; i->openmax = v->openmax; changed = 1; }
    else if (i->max == v->max && !i->openmax && v->openmax) { i->openmax = 1; changed = 1; }
    if (!i->integer && v->integer) { i->integer = 1; changed = 1; }
    if (i->integer) {
        if (i->openmin) { i->min++; i->openmin = 0; }
        if (i->openmax) { i->max--; i->openmax = 0; }
    } else if (!i->openmin && !i->openmax && i->min == i->max) i->integer = 1;
    if (ivl_empty(i)) { i->empty = 1; return -EINVAL_; }
    return changed;
}

static int refine_mm(struct ivl *i, uint32_t min, uint32_t max, int integer)
{
    struct ivl v = { min, max, 0, 0, (uint32_t)integer, 0 };
    return refine(i, &v);
}

static void iv_mul(const struct ivl *a, const struct ivl *b, struct ivl *c)
{
    if (a->empty || b->empty) { c->empty = 1; return; }
    c->empty = 0;
    c->min = mul32(a->min, b->min); c->openmin = a->openmin || b->openmin;
    c->max = mul32(a->max, b->max); c->openmax = a->openmax || b->openmax;
    c->integer = a->integer && b->integer;
}

static void iv_div(const struct ivl *a, const struct ivl *b, struct ivl *c)
{
    uint32_t r;
    if (a->empty || b->empty) { c->empty = 1; return; }
    c->empty = 0;
    c->min = div32(a->min, b->max, &r);
    c->openmin = r || a->openmin || b->openmax;
    if (b->min > 0) {
        c->max = div32(a->max, b->min, &r);
        if (r) { c->max++; c->openmax = 1; } else c->openmax = a->openmax || b->openmin;
    } else { c->max = 0xFFFFFFFFu; c->openmax = 0; }
    c->integer = 0;
}

static void iv_muldivk(const struct ivl *a, const struct ivl *b, uint32_t k, struct ivl *c)
{
    uint32_t r;
    if (a->empty || b->empty) { c->empty = 1; return; }
    c->empty = 0;
    c->min = muldiv32(a->min, b->min, k, &r);
    c->openmin = r || a->openmin || b->openmin;
    c->max = muldiv32(a->max, b->max, k, &r);
    if (r) { c->max++; c->openmax = 1; } else c->openmax = a->openmax || b->openmax;
    c->integer = 0;
}

static void iv_mulkdiv(const struct ivl *a, uint32_t k, const struct ivl *b, struct ivl *c)
{
    uint32_t r;
    if (a->empty || b->empty) { c->empty = 1; return; }
    c->empty = 0;
    c->min = muldiv32(a->min, k, b->max, &r);
    c->openmin = r || a->openmin || b->openmax;
    if (b->min > 0) {
        c->max = muldiv32(a->max, k, b->min, &r);
        if (r) { c->max++; c->openmax = 1; } else c->openmax = a->openmax || b->openmin;
    } else { c->max = 0xFFFFFFFFu; c->openmax = 0; }
    c->integer = 0;
}

/* rate is one of a list: snd_interval_list */
static int refine_list(struct ivl *i, const uint32_t *list, int n)
{
    struct ivl v = { 0xFFFFFFFFu, 0, 0, 0, 1, 0 };
    int any = 0;
    for (int k = 0; k < n; k++) {
        uint32_t x = list[k];
        if (x < i->min || x > i->max || (x == i->min && i->openmin) || (x == i->max && i->openmax)) continue;
        if (x < v.min) v.min = x;
        if (x > v.max) v.max = x;
        any = 1;
    }
    if (!any) { i->empty = 1; return -EINVAL_; }
    return refine(i, &v);
}

/* a value that must be a multiple of step (period bytes: HDA buffers start 128-aligned) */
static int refine_step(struct ivl *i, uint32_t step)
{
    struct ivl v = *i;
    uint32_t mn = v.min + (v.openmin ? 1 : 0), mx = v.max - (v.openmax && v.max ? 1 : 0);
    mn = (mn + step - 1) / step * step;
    mx = mx / step * step;
    v.min = mn; v.max = mx; v.openmin = v.openmax = 0; v.integer = 1;
    if (mn > mx) { i->empty = 1; return -EINVAL_; }
    return refine(i, &v);
}

static int mask_empty(const uint32_t *m) { for (int i = 0; i < 8; i++) if (m[i]) return 0; return 1; }
static int mask_refine(uint32_t *m, const uint32_t *allow)
{
    int changed = 0;
    for (int i = 0; i < 8; i++) { uint32_t o = m[i]; m[i] &= allow[i]; if (m[i] != o) changed = 1; }
    return mask_empty(m) ? -EINVAL_ : changed;
}

static int snd_debug = 0;
static void dump(const char *why, const struct hwp *p)
{
    kprintf("snd: %s: fmt %x acc %x", why, p->masks[1][0], p->masks[0][0]);
    static const char *const nm[] = { "sbits", "fbits", "ch", "rate", "ptime", "psize", "pbytes", "periods", "btime", "bsize", "bbytes", "tick" };
    for (int k = 0; k < 12; k++) { const struct ivl *i = &p->iv[k]; kprintf(" %s%s%u,%u%s%s", nm[k], i->openmin ? "(" : "[", i->min, i->max, i->openmax ? ")" : "]", i->empty ? "E" : i->integer ? "i" : ""); }
    kprintf("\n");
}
#define CH(k, e) do { int r_ = (e); if (r_ < 0) { if (snd_debug) { kprintf("snd: refine fails at %d (pass %d)\n", k, pass); dump("now", p); } return r_; } if (r_) changed |= 1u << (k); } while (0)

/* the hardware's limits, then ALSA's rules between the parameters, until nothing moves */
static int hw_refine(struct hwp *p)
{
    uint32_t changed = 0;
    int pass = -1;
    if (snd_debug) dump("in", p);
    uint32_t acc[8] = { 1u << ACCESS_RW_INTERLEAVED | 1u << 0 /* MMAP_INTERLEAVED */ }, fmt[8] = { 1u << FORMAT_S16_LE }, sub[8] = { 1 };
    CH(P_ACCESS, mask_refine(p->masks[0], acc));
    CH(P_FORMAT, mask_refine(p->masks[1], fmt));
    CH(P_SUBFORMAT, mask_refine(p->masks[2], sub));
    static const uint32_t rates[] = { RATE_A, RATE_B };
    for (pass = 0; pass < 32; pass++) {
        uint32_t before = changed;
        struct ivl t;
        CH(P_SAMPLE_BITS, refine_mm(IV(p, P_SAMPLE_BITS), 16, 16, 1));
        CH(P_CHANNELS, refine_mm(IV(p, P_CHANNELS), 2, 2, 1));
        CH(P_RATE, refine_list(IV(p, P_RATE), rates, 2));
        CH(P_PERIOD_BYTES, refine_mm(IV(p, P_PERIOD_BYTES), PERIOD_BYTES_MIN, PERIOD_BYTES_MAX, 1));
        CH(P_PERIOD_BYTES, refine_step(IV(p, P_PERIOD_BYTES), 128));
        CH(P_PERIODS, refine_mm(IV(p, P_PERIODS), PERIODS_MIN, PERIODS_MAX, 1));
        CH(P_BUFFER_BYTES, refine_mm(IV(p, P_BUFFER_BYTES), 8192, BUFFER_BYTES_MAX, 1));
        CH(P_PERIOD_SIZE, refine_mm(IV(p, P_PERIOD_SIZE), 1, 0xFFFFFFFFu, 1));
        CH(P_BUFFER_SIZE, refine_mm(IV(p, P_BUFFER_SIZE), 1, 0xFFFFFFFFu, 1));
        /* FRAME_BITS */
        iv_mul(IV(p, P_SAMPLE_BITS), IV(p, P_CHANNELS), &t); CH(P_FRAME_BITS, refine(IV(p, P_FRAME_BITS), &t));
        iv_mulkdiv(IV(p, P_PERIOD_BYTES), 8, IV(p, P_PERIOD_SIZE), &t); CH(P_FRAME_BITS, refine(IV(p, P_FRAME_BITS), &t));
        iv_mulkdiv(IV(p, P_BUFFER_BYTES), 8, IV(p, P_BUFFER_SIZE), &t); CH(P_FRAME_BITS, refine(IV(p, P_FRAME_BITS), &t));
        CH(P_FRAME_BITS, refine_mm(IV(p, P_FRAME_BITS), 1, 0xFFFFFFFFu, 1));
        /* CHANNELS, SAMPLE_BITS */
        iv_div(IV(p, P_FRAME_BITS), IV(p, P_SAMPLE_BITS), &t); CH(P_CHANNELS, refine(IV(p, P_CHANNELS), &t));
        iv_div(IV(p, P_FRAME_BITS), IV(p, P_CHANNELS), &t); CH(P_SAMPLE_BITS, refine(IV(p, P_SAMPLE_BITS), &t));
        /* RATE */
        iv_mulkdiv(IV(p, P_PERIOD_SIZE), 1000000, IV(p, P_PERIOD_TIME), &t); CH(P_RATE, refine(IV(p, P_RATE), &t));
        iv_mulkdiv(IV(p, P_BUFFER_SIZE), 1000000, IV(p, P_BUFFER_TIME), &t); CH(P_RATE, refine(IV(p, P_RATE), &t));
        CH(P_RATE, refine_list(IV(p, P_RATE), rates, 2));
        /* PERIODS */
        iv_div(IV(p, P_BUFFER_SIZE), IV(p, P_PERIOD_SIZE), &t); CH(P_PERIODS, refine(IV(p, P_PERIODS), &t));
        /* PERIOD_SIZE */
        iv_div(IV(p, P_BUFFER_SIZE), IV(p, P_PERIODS), &t); CH(P_PERIOD_SIZE, refine(IV(p, P_PERIOD_SIZE), &t));
        iv_mulkdiv(IV(p, P_PERIOD_BYTES), 8, IV(p, P_FRAME_BITS), &t); CH(P_PERIOD_SIZE, refine(IV(p, P_PERIOD_SIZE), &t));
        iv_muldivk(IV(p, P_PERIOD_TIME), IV(p, P_RATE), 1000000, &t); CH(P_PERIOD_SIZE, refine(IV(p, P_PERIOD_SIZE), &t));
        /* BUFFER_SIZE */
        iv_mul(IV(p, P_PERIOD_SIZE), IV(p, P_PERIODS), &t); CH(P_BUFFER_SIZE, refine(IV(p, P_BUFFER_SIZE), &t));
        iv_mulkdiv(IV(p, P_BUFFER_BYTES), 8, IV(p, P_FRAME_BITS), &t); CH(P_BUFFER_SIZE, refine(IV(p, P_BUFFER_SIZE), &t));
        iv_muldivk(IV(p, P_BUFFER_TIME), IV(p, P_RATE), 1000000, &t); CH(P_BUFFER_SIZE, refine(IV(p, P_BUFFER_SIZE), &t));
        /* bytes */
        iv_muldivk(IV(p, P_PERIOD_SIZE), IV(p, P_FRAME_BITS), 8, &t); CH(P_PERIOD_BYTES, refine(IV(p, P_PERIOD_BYTES), &t));
        iv_muldivk(IV(p, P_BUFFER_SIZE), IV(p, P_FRAME_BITS), 8, &t); CH(P_BUFFER_BYTES, refine(IV(p, P_BUFFER_BYTES), &t));
        /* times */
        iv_mulkdiv(IV(p, P_PERIOD_SIZE), 1000000, IV(p, P_RATE), &t); CH(P_PERIOD_TIME, refine(IV(p, P_PERIOD_TIME), &t));
        iv_mulkdiv(IV(p, P_BUFFER_SIZE), 1000000, IV(p, P_RATE), &t); CH(P_BUFFER_TIME, refine(IV(p, P_BUFFER_TIME), &t));
        if (changed == before && pass) break;
    }
    p->cmask |= changed;
    p->rmask = 0;
    p->info = INFO;
    if (ivl_single(IV(p, P_SAMPLE_BITS))) p->msbits = 16;
    if (ivl_single(IV(p, P_RATE))) { p->rate_num = ivl_value(IV(p, P_RATE)); p->rate_den = 1; }
    p->fifo_size = 0;
    return 0;
}

/* ── the stream ─────────────────────────────────────────────────────── */
enum { ST_OPEN, ST_SETUP, ST_PREPARED, ST_RUNNING, ST_XRUN, ST_DRAINING, ST_PAUSED, ST_SUSPENDED, ST_DISCONNECTED };
static struct {
    struct file *owner;
    int state;
    uint32_t rate, channels, fb;                 /* frame bytes */
    uint64_t period, periods, bsize;             /* frames */
    uint64_t hw, appl, boundary;                 /* frames, never wrapped (boundary is far away) */
    uint32_t last_pos;
    uint64_t avail_min, start_threshold, stop_threshold;
    uint64_t trigger_ns;
    uint8_t *ring; uint32_t ring_cap;
    struct node *node;
} pcm;

/* mmap of the PCM's data (offset 0): the ring itself. (Status and control aren't mapped:
   alsa-lib then keeps them in step with SYNC_PTR.) */
struct node *snd_mmap_node(struct file *f, uint64_t off, uint64_t len)
{
    if (pcm.owner != f || !pcm.node || off != 0 || len > pcm.ring_cap) return 0;
    return pcm.node;
}

static uint64_t mono(void) { return mono_ns(); }

static void stop(int state)
{
    hda_stop();
    pcm.state = state;
}

/* bring hw up to where the controller is; zero what it has played (an underrun plays silence) */
static void update(void)
{
    if ((pcm.state != ST_RUNNING && pcm.state != ST_DRAINING) || !pcm.bsize) return;
    uint32_t bb = (uint32_t)(pcm.bsize * pcm.fb), pos = hda_position();
    pos -= pos % pcm.fb;
    uint32_t delta = (pos + bb - pcm.last_pos) % bb;
    if (delta) {
        uint32_t from = pcm.last_pos;
        for (uint32_t done = 0; done < delta; ) {
            uint32_t k = bb - from < delta - done ? bb - from : delta - done;
            memset(pcm.ring + from, 0, k);
            from = (from + k) % bb; done += k;
        }
        pcm.last_pos = pos;
        pcm.hw += delta / pcm.fb;
    }
    if (pcm.state == ST_DRAINING && pcm.hw >= pcm.appl) { pcm.hw = pcm.appl; stop(ST_SETUP); return; }
    uint64_t avail = pcm.hw + pcm.bsize - pcm.appl;
    if (pcm.hw > pcm.appl + pcm.bsize) avail = 2 * pcm.bsize;
    if (pcm.state == ST_RUNNING && avail >= pcm.stop_threshold) stop(ST_XRUN);
}

static long avail(void) { long a = (long)(pcm.hw + pcm.bsize) - (long)pcm.appl; return a < 0 ? 0 : a; }

static int start(void)
{
    if (pcm.state != ST_PREPARED) return -EBADFD_;
    if (pcm.appl == pcm.hw) return -EPIPE_;
    pcm.last_pos = 0;
    if (hda_start((int)pcm.rate, (int)pcm.channels, (uint32_t)(pcm.bsize * pcm.fb), (uint32_t)(pcm.period * pcm.fb)) < 0) return -EIO_;
    pcm.state = ST_RUNNING;
    pcm.trigger_ns = mono();
    return 0;
}

static int signalled(struct task *t) { return (t->pending & ~t->sigmask) != 0; }

static long writei(struct task *t, struct file *f, uint64_t ubuf, uint64_t frames)
{
    if (pcm.state == ST_XRUN) return -EPIPE_;
    if (pcm.state == ST_SUSPENDED) return -ESTRPIPE_;
    if (pcm.state != ST_PREPARED && pcm.state != ST_RUNNING) return -EBADFD_;
    uint64_t done = 0;
    while (done < frames) {
        update();
        if (pcm.state == ST_XRUN) return done ? (long)done : -EPIPE_;
        long a = avail();
        if (a <= 0) {
            if (f->flags & 04000) return done ? (long)done : -EAGAIN_;
            if (signalled(t)) return done ? (long)done : -EINTR_;
            wait_tick();
            continue;
        }
        uint64_t n = frames - done < (uint64_t)a ? frames - done : (uint64_t)a;
        uint64_t at = pcm.appl % pcm.bsize, room = pcm.bsize - at;
        if (n > room) n = room;
        if (copyin(t->mm, pcm.ring + at * pcm.fb, ubuf + done * pcm.fb, n * pcm.fb) < 0) return done ? (long)done : -EFAULT_;
        pcm.appl += n;
        done += n;
        if (pcm.state == ST_PREPARED && pcm.appl - pcm.hw >= pcm.start_threshold) { int r = start(); if (r < 0) return done ? (long)done : r; }
    }
    return (long)done;
}

/* struct snd_pcm_info */
static void pcm_info(uint8_t *b)
{
    memset(b, 0, 288);
    int stream = 0, card = 0;
    memcpy(b + 8, &stream, 4); memcpy(b + 12, &card, 4);
    strlcpy((char *)b + 16, "HDA Analog", 64);
    strlcpy((char *)b + 80, "Auxo HDA Analog", 80);
    strlcpy((char *)b + 160, "subdevice #0", 32);
    uint32_t one = 1;
    memcpy(b + 200, &one, 4); memcpy(b + 204, &one, 4);       /* subdevices count, avail */
}

/* struct snd_pcm_status (152 bytes) */
static void status(uint8_t *b)
{
    update();
    memset(b, 0, 152);
    int st = pcm.state;
    memcpy(b, &st, 4);
    int64_t ts[2] = { (int64_t)(pcm.trigger_ns / 1000000000ULL), (int64_t)(pcm.trigger_ns % 1000000000ULL) };
    memcpy(b + 8, ts, 16);
    uint64_t n = mono(); int64_t now[2] = { (int64_t)(n / 1000000000ULL), (int64_t)(n % 1000000000ULL) };
    memcpy(b + 24, now, 16);
    uint64_t ap = pcm.appl % pcm.boundary, hp = pcm.hw % pcm.boundary;
    long delay = (long)(pcm.appl - pcm.hw), av = avail();
    if (pcm.state != ST_RUNNING && pcm.state != ST_DRAINING) delay = 0;
    memcpy(b + 40, &ap, 8); memcpy(b + 48, &hp, 8); memcpy(b + 56, &delay, 8); memcpy(b + 64, &av, 8); memcpy(b + 72, &av, 8);
}

/* struct snd_pcm_sync_ptr (136): flags, then the status and control "mmap" areas */
static long sync_ptr(struct task *t, uint64_t arg)
{
    uint8_t b[136];
    if (copyin(t->mm, b, arg, sizeof b) < 0) return -EFAULT_;
    uint32_t flags; memcpy(&flags, b, 4);
    if (flags & 1) update();                                  /* HWSYNC */
    if (!(flags & 2)) {                                       /* APPL: take the program's appl_ptr */
        uint64_t ap; memcpy(&ap, b + 72, 8);
        if (pcm.boundary) {
            uint64_t cur = pcm.appl % pcm.boundary;
            long d = (long)ap - (long)cur;                    /* forward or back (rewind), within reason */
            if (d && d > -(long)pcm.bsize && d <= (long)pcm.bsize) pcm.appl += (uint64_t)d;
        }
    }
    if (!(flags & 4)) { uint64_t am; memcpy(&am, b + 80, 8); if (am) pcm.avail_min = am; }   /* AVAIL_MIN */
    update();
    int st = pcm.state;
    memset(b + 8, 0, 64);
    memcpy(b + 8, &st, 4);
    uint64_t hp = pcm.boundary ? pcm.hw % pcm.boundary : 0, ap = pcm.boundary ? pcm.appl % pcm.boundary : 0;
    memcpy(b + 16, &hp, 8);
    uint64_t n = mono(); int64_t now[2] = { (int64_t)(n / 1000000000ULL), (int64_t)(n % 1000000000ULL) };
    memcpy(b + 24, now, 16);
    memcpy(b + 72, &ap, 8);
    memcpy(b + 80, &pcm.avail_min, 8);
    return copyout(t->mm, arg, b, sizeof b) < 0 ? -EFAULT_ : 0;
}

static long pcm_ioctl(struct task *t, struct file *f, uint64_t req, uint64_t arg)
{
    switch (req) {
    case 0x80044100: { int v = 0x0002000E; return copyout(t->mm, arg, &v, 4); }       /* PVERSION 2.0.14 */
    case 0x81204101: { uint8_t b[288]; pcm_info(b); return copyout(t->mm, arg, b, sizeof b); }
    case 0x40044102: case 0x40044103: case 0x40044104: return 0;                     /* TSTAMP, TTSTAMP, USER_PVERSION */
    case 0xC2604110: case 0xC2604111: {                                              /* HW_REFINE, HW_PARAMS */
        struct hwp p;
        if (copyin(t->mm, &p, arg, sizeof p) < 0) return -EFAULT_;
        if (req == 0xC2604111 && pcm.state != ST_OPEN && pcm.state != ST_SETUP && pcm.state != ST_PREPARED) return -EBADFD_;
        p.cmask = 0;
        int r = hw_refine(&p);
        if (r < 0) return r;
        if (req == 0xC2604111) {
            /* all of it must be decided now: take the smallest of whatever's still open */
            for (int k = P_SAMPLE_BITS; k <= P_LAST; k++) if (!ivl_single(IV(&p, k))) { struct ivl *i = IV(&p, k); i->max = i->min; i->openmax = 0; r = hw_refine(&p); if (r < 0) return r; }
            pcm.rate = ivl_value(IV(&p, P_RATE));
            pcm.channels = ivl_value(IV(&p, P_CHANNELS));
            pcm.fb = ivl_value(IV(&p, P_FRAME_BITS)) / 8;
            pcm.period = ivl_value(IV(&p, P_PERIOD_SIZE));
            pcm.periods = ivl_value(IV(&p, P_PERIODS));
            pcm.bsize = pcm.period * pcm.periods;
            if (!pcm.fb || !pcm.bsize || pcm.bsize * pcm.fb > pcm.ring_cap) return -EINVAL_;
            pcm.boundary = pcm.bsize;
            while (pcm.boundary * 2 <= 0x7FFFFFFFFFFFFFFFULL - pcm.bsize) pcm.boundary *= 2;
            pcm.avail_min = pcm.period; pcm.start_threshold = 1; pcm.stop_threshold = pcm.bsize;
            pcm.hw = pcm.appl = 0;
            stop(ST_SETUP);
        }
        return copyout(t->mm, arg, &p, sizeof p) < 0 ? -EFAULT_ : 0;
    }
    case 0x4112: if (pcm.state == ST_RUNNING) return -EBADFD_; stop(ST_OPEN); return 0;   /* HW_FREE */
    case 0xC0884113: {                                                                 /* SW_PARAMS */
        uint8_t b[136];
        if (pcm.state == ST_OPEN) return -EBADFD_;
        if (copyin(t->mm, b, arg, sizeof b) < 0) return -EFAULT_;
        uint64_t am, st, sp;
        memcpy(&am, b + 16, 8); memcpy(&st, b + 32, 8); memcpy(&sp, b + 40, 8);
        pcm.avail_min = am ? am : 1;
        pcm.start_threshold = st ? st : 1;
        pcm.stop_threshold = sp;
        memcpy(b + 64, &pcm.boundary, 8);
        return copyout(t->mm, arg, b, sizeof b) < 0 ? -EFAULT_ : 0;
    }
    case 0x80984120: case 0xC0984124: { uint8_t b[152]; status(b); return copyout(t->mm, arg, b, sizeof b); }   /* STATUS, STATUS_EXT */
    case 0x80084121: {                                                                 /* DELAY */
        update();
        if (pcm.state == ST_XRUN) return -EPIPE_;
        long d = (long)(pcm.appl - pcm.hw);
        if (d < 0) d = 0;
        return copyout(t->mm, arg, &d, 8);
    }
    case 0x4122: update(); return pcm.state == ST_XRUN ? -EPIPE_ : 0;                     /* HWSYNC */
    case 0xC0884123: return sync_ptr(t, arg);
    case 0x80184132: {                                                                   /* CHANNEL_INFO (mmap) */
        uint8_t b[24];
        if (copyin(t->mm, b, arg, sizeof b) < 0) return -EFAULT_;
        uint32_t ch; memcpy(&ch, b, 4);
        if (ch >= pcm.channels) return -EINVAL_;
        int64_t off = 0; uint32_t first = ch * 16, step = pcm.fb * 8;
        memcpy(b + 8, &off, 8); memcpy(b + 16, &first, 4); memcpy(b + 20, &step, 4);
        return copyout(t->mm, arg, b, sizeof b);
    }
    case 0x4140:                                                                         /* PREPARE */
        if (pcm.state == ST_OPEN || pcm.state == ST_DISCONNECTED) return -EBADFD_;
        hda_stop();
        pcm.hw = (pcm.hw + pcm.bsize - 1) / pcm.bsize * pcm.bsize;   /* a buffer boundary: the controller starts at the ring's start */
        pcm.appl = pcm.hw;
        memset(pcm.ring, 0, pcm.bsize * pcm.fb);
        pcm.state = ST_PREPARED;
        return 0;
    case 0x4141: update(); pcm.appl = pcm.hw; return 0;                                 /* RESET */
    case 0x4142: return start();                                                         /* START */
    case 0x4143: if (pcm.state == ST_OPEN) return -EBADFD_; update(); stop(ST_SETUP); return 0;   /* DROP */
    case 0x4144:                                                                         /* DRAIN */
        if (pcm.state == ST_OPEN) return -EBADFD_;
        if (pcm.state == ST_PREPARED) { if (pcm.appl > pcm.hw) { int r = start(); if (r < 0) return r; } else { stop(ST_SETUP); return 0; } }
        if (pcm.state == ST_XRUN) { stop(ST_SETUP); return 0; }
        if (pcm.state != ST_RUNNING) return 0;
        pcm.state = ST_DRAINING;
        if (f->flags & 04000) return -EAGAIN_;
        while (pcm.state == ST_DRAINING) { if (signalled(t)) return -EINTR_; wait_tick(); update(); }
        return 0;
    case 0x40044145: return -ENOSYS_SND;                                                 /* PAUSE: not offered (info) */
    case 0x40084146: {                                                                   /* REWIND */
        uint64_t n; if (copyin(t->mm, &n, arg, 8) < 0) return -EFAULT_;
        update();
        uint64_t can = pcm.appl > pcm.hw ? pcm.appl - pcm.hw : 0;
        if (n > can) n = can;
        pcm.appl -= n;
        return (long)n;
    }
    case 0x40084149: {                                                                   /* FORWARD */
        uint64_t n; if (copyin(t->mm, &n, arg, 8) < 0) return -EFAULT_;
        update();
        long a = avail();
        if ((long)n > a) n = (uint64_t)a;
        pcm.appl += n;
        return (long)n;
    }
    case 0x4147: return -ENOSYS_SND;                                                     /* RESUME */
    case 0x4148: if (pcm.state == ST_RUNNING) stop(ST_XRUN); return 0;                    /* XRUN */
    case 0x40184150: {                                                                   /* WRITEI_FRAMES */
        int64_t x[3];
        if (copyin(t->mm, x, arg, 24) < 0) return -EFAULT_;
        long r = writei(t, f, (uint64_t)x[1], (uint64_t)x[2]);
        x[0] = r;
        copyout(t->mm, arg, x, 8);
        return r < 0 ? r : 0;
    }
    case 0x80184151: case 0x40184152: case 0x80184153: return -EINVAL_;                 /* capture, non-interleaved */
    case 0x40044160: case 0x4161: return -ENOSYS_SND;                                     /* LINK, UNLINK */
    }
    return -ENOTTY_;
}

/* ── the control device ─────────────────────────────────────────────── */
static const char *const ELEM[2] = { "Master Playback Volume", "Master Playback Switch" };

static void elem_id(uint8_t *b, int k)
{
    memset(b, 0, 64);
    uint32_t numid = (uint32_t)k + 1, iface = 2;            /* SNDRV_CTL_ELEM_IFACE_MIXER */
    memcpy(b, &numid, 4); memcpy(b + 4, &iface, 4);
    strlcpy((char *)b + 16, ELEM[k], 44);
}

static int find_elem(const uint8_t *id)
{
    uint32_t numid; memcpy(&numid, id, 4);
    if (numid) return numid >= 1 && numid <= 2 ? (int)numid - 1 : -1;
    for (int k = 0; k < 2; k++) if (!strncmp((const char *)id + 16, ELEM[k], 44)) return k;
    return -1;
}

static long ctl_ioctl(struct task *t, uint64_t req, uint64_t arg)
{
    switch (req) {
    case 0x80045500: { int v = 0x00020008; return copyout(t->mm, arg, &v, 4); }       /* PVERSION 2.0.8 */
    case 0x81785501: {                                                                 /* CARD_INFO */
        uint8_t b[376];
        memset(b, 0, sizeof b);
        strlcpy((char *)b + 8, "Intel", 16);
        strlcpy((char *)b + 24, "AuxoHDA", 16);
        strlcpy((char *)b + 40, "HDA Intel", 32);
        ksnprintf((char *)b + 72, 80, "HDA Intel (codec %s, %s)", hda_codec_name(), hda_output_name());
        ksnprintf((char *)b + 168, 80, "Codec %s", hda_codec_name());
        ksnprintf((char *)b + 248, 128, "HDA:%s", hda_codec_name());
        return copyout(t->mm, arg, b, sizeof b);
    }
    case 0xC0505510: {                                                                 /* ELEM_LIST */
        uint8_t b[80];
        if (copyin(t->mm, b, arg, sizeof b) < 0) return -EFAULT_;
        uint32_t off, space, used = 0, count = 2;
        uint64_t pids;
        memcpy(&off, b, 4); memcpy(&space, b + 4, 4); memcpy(&pids, b + 16, 8);
        for (uint32_t i = off; i < 2 && used < space; i++, used++) {
            uint8_t id[64]; elem_id(id, (int)i);
            if (copyout(t->mm, pids + used * 64, id, 64) < 0) return -EFAULT_;
        }
        memcpy(b + 8, &used, 4); memcpy(b + 12, &count, 4);
        return copyout(t->mm, arg, b, sizeof b);
    }
    case 0xC1105511: {                                                                 /* ELEM_INFO */
        uint8_t b[272];
        if (copyin(t->mm, b, arg, sizeof b) < 0) return -EFAULT_;
        int k = find_elem(b);
        if (k < 0) return -ENOENT_;
        memset(b, 0, sizeof b);
        elem_id(b, k);
        int type = k == 0 ? 2 : 1;                              /* INTEGER, BOOLEAN */
        uint32_t access = 3, count = 2;
        memcpy(b + 64, &type, 4); memcpy(b + 68, &access, 4); memcpy(b + 72, &count, 4);
        int64_t mn = 0, mx = k == 0 ? 100 : 1, step = k == 0 ? 1 : 0;
        memcpy(b + 80, &mn, 8); memcpy(b + 88, &mx, 8); memcpy(b + 96, &step, 8);
        return copyout(t->mm, arg, b, sizeof b);
    }
    case 0xC4C85512: case 0xC4C85513: {                                                /* ELEM_READ, ELEM_WRITE */
        static uint8_t b[1224];
        if (copyin(t->mm, b, arg, sizeof b) < 0) return -EFAULT_;
        int k = find_elem(b);
        if (k < 0) return -ENOENT_;
        if (req == 0xC4C85513) {
            int64_t v[2]; memcpy(v, b + 72, 16);
            if (k == 0) hda_set_volume((int)((v[0] + v[1]) / 2), hda_muted());
            else hda_set_volume(hda_volume(), !(v[0] || v[1]));
            return 0;
        }
        int64_t v = k == 0 ? hda_volume() : !hda_muted();
        memcpy(b + 72, &v, 8); memcpy(b + 80, &v, 8);
        return copyout(t->mm, arg, b, sizeof b);
    }
    case 0xC0045516: { int v = 0; return copyout(t->mm, arg, &v, 4); }                 /* SUBSCRIBE_EVENTS: none */
    case 0x80045530: {                                                                 /* PCM_NEXT_DEVICE */
        int d; if (copyin(t->mm, &d, arg, 4) < 0) return -EFAULT_;
        d = d < 0 ? 0 : -1;
        return copyout(t->mm, arg, &d, 4);
    }
    case 0xC1205531: {                                                                 /* PCM_INFO */
        uint8_t b[288];
        if (copyin(t->mm, b, arg, sizeof b) < 0) return -EFAULT_;
        uint32_t dev, sub; int stream;
        memcpy(&dev, b, 4); memcpy(&sub, b + 4, 4); memcpy(&stream, b + 8, 4);
        if (dev != 0 || stream != 0) return -ENOENT_;          /* playback only */
        pcm_info(b);
        return copyout(t->mm, arg, b, sizeof b);
    }
    case 0x40045532: return 0;                                                         /* PCM_PREFER_SUBDEVICE */
    case 0x80045520: case 0x80045540: case 0x80045550: {                               /* hwdep, rawmidi, ump: none */
        int d = -1; return copyout(t->mm, arg, &d, 4);
    }
    case 0x800455D1: { int v = 0; return copyout(t->mm, arg, &v, 4); }                 /* POWER_STATE: D0 */
    }
    return -ENOTTY_;
}

/* ── files ──────────────────────────────────────────────────────────── */
int snd_open(struct file *f)
{
    if (!f->node || f->node->dev != DEV_SND_PCM) return 0;
    if (pcm.owner && pcm.owner != f) return -EBUSY_;
    pcm.owner = f;
    pcm.state = ST_OPEN;
    return 0;
}

void snd_close(struct file *f)
{
    if (pcm.owner != f) return;
    stop(ST_OPEN);
    pcm.owner = 0;
}

long snd_ioctl(struct task *t, struct file *f, uint64_t req, uint64_t arg)
{
    if (f->node->dev == DEV_SND_CTL) return ctl_ioctl(t, req, arg);
    if (pcm.owner != f) return -EBADFD_;
    return pcm_ioctl(t, f, req, arg);
}

/* poll: 4 writable (avail_min frames free), 8 error (an xrun, or not set up) */
int snd_ready(struct file *f)
{
    if (f->node->dev == DEV_SND_CTL) return 0;
    if (pcm.owner != f) return 8;
    update();
    if (pcm.state == ST_XRUN || pcm.state == ST_OPEN || pcm.state == ST_SETUP) return 8 | 4;
    return avail() >= (long)pcm.avail_min ? 4 : 0;
}

uint64_t snd_rdev(struct node *n) { return n->dev == DEV_SND_CTL ? (116u << 8) | 0 : (116u << 8) | 16; }

/* /dev/snd: made when there's a sound card. In a thread of its own, after the boot: a sound
   chip that's slow to answer (or one we don't understand) must never hold up the desktop. */
static void snd_thread(uintptr_t arg)
{
    (void)arg;
    if (hda_init() < 0) return;
    pcm.ring = hda_ring(&pcm.ring_cap);
    /* the ring as a file of its own, for mmap: its pages are the controller's, never moved or freed */
    static struct node ring_node;
    ring_node.type = N_REG; ring_node.mode = 0600; ring_node.name = "snd-ring";
    ring_node.data = pcm.ring; ring_node.size = ring_node.cap = pcm.ring_cap;
    ring_node.refs = 1; ring_node.maps = 1; ring_node.nlink = 1; ring_node.parent = &ring_node;
    pcm.node = &ring_node;
    struct node *dev, *d;
    if (fs_lookup(0, "/dev", 1, &dev) < 0) return;
    d = fs_child(dev, "snd") ? fs_child(dev, "snd") : fs_create(dev, "snd", N_DIR, 0755);
    if (!d) return;
    static const struct { const char *n; int dev; } nodes[] = { { "controlC0", DEV_SND_CTL }, { "pcmC0D0p", DEV_SND_PCM } };
    for (int i = 0; i < 2; i++) {
        struct node *n = fs_child(d, nodes[i].n) ? fs_child(d, nodes[i].n) : fs_create(d, nodes[i].n, N_CHR, 0660);
        if (n) { n->dev = (uint32_t)nodes[i].dev; n->mode = 0660; n->uid = 0; n->gid = 29; n->changed = 0; }   /* group audio */
    }
    kprintf("snd : HDA Intel, codec %s, playing on the %s: /dev/snd (ALSA)\n", hda_codec_name(), hda_output_name());
}

void snd_init(void) { rs_spawn("snd", snd_thread, 0); }
