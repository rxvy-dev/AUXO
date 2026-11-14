/* System Monitor: what the computer is doing and what it is.
 *   Processes  every program with its user, CPU and memory; end or kill the one you pick.
 *   Resources  CPU, memory, network and disk over the last two minutes.
 *   System     the hardware: processor, memory, graphics, disks, network, sound, every PCI device.
 * Samples once a second while the window is open. CPU time comes from the scheduler's tick
 * counts (the idle thread's share is the free CPU); a program's memory is its resident set,
 * counted from its page tables. Runs on the Chrono thread (SSE), like the other built-in apps. */
#include "kern.h"
#include "arch.h"
#include "gfx.h"
#include "fonts.h"
#include "apps.h"
#include "rs.h"
#include "uproc.h"
#include "cred.h"
#include "pci.h"
#include "disk.h"
#include "net.h"
#include "hda.h"
#include "auxo.h"
#include "auxofs.h"

#define HIST 120
#define MAXP 256
#define ROW 26
#define TABS 52

enum { TAB_PROC, TAB_RES, TAB_SYS };
static int tab;

/* ── sampling ───────────────────────────────────────────────────────── */
struct prow {
    int pid, threads, kernel;
    char name[16], user[20], exe[128];
    uint32_t uid;
    uint64_t ticks, rss;          /* rss in pages */
    int cpu10;                    /* CPU use, tenths of a percent */
};
static struct prow procs[MAXP], prev[MAXP];
static int nprocs, nprev, nthreads;

static uint64_t last_sample, last_total, last_idle, last_rx, last_tx;
static int sampled;
static uint8_t h_cpu[HIST], h_mem[HIST];           /* percent */
static uint32_t h_rx[HIST], h_tx[HIST];            /* bytes per second */
static int hn;                                     /* samples in history */
static int cpu_now;                                /* tenths of a percent */
static uint64_t rx_rate, tx_rate, prog_pages;

struct th { int pid, state; uint64_t ticks; char name[16]; };
static struct th ths[512];
static int nths;

static struct th *th_find(int pid)
{
    int lo = 0, hi = nths - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (ths[m].pid == pid) return &ths[m];
        if (ths[m].pid < pid) lo = m + 1; else hi = m - 1;
    }
    return 0;
}

static struct { uint32_t uid; char name[20]; } ucache[16];
static int nucache;

static void user_name(uint32_t uid, char *out, size_t n)
{
    for (int i = 0; i < nucache; i++) if (ucache[i].uid == uid) { strlcpy(out, ucache[i].name, n); return; }
    struct account a;
    char nm[20];
    if (account_by_uid(uid, &a) == 0) strlcpy(nm, a.name, sizeof nm);
    else ksnprintf(nm, sizeof nm, "%u", uid);
    if (nucache < 16) { ucache[nucache].uid = uid; strlcpy(ucache[nucache].name, nm, sizeof ucache[0].name); nucache++; }
    strlcpy(out, nm, n);
}

static void push(uint8_t *h, int v) { if (hn == HIST) memmove(h, h + 1, HIST - 1); h[hn == HIST ? HIST - 1 : hn] = (uint8_t)v; }
static void push32(uint32_t *h, uint32_t v) { if (hn == HIST) memmove(h, h + 1, (HIST - 1) * 4); h[hn == HIST ? HIST - 1 : hn] = v; }

static void sample(void)
{
    uint64_t now = kernel_ticks();
    /* the scheduler's view: every thread, sorted by pid */
    nths = 0;
    uint64_t idle = 0, total = 0;
    for (size_t i = 0; nths < 512; i++) {
        struct th t;
        int st = rs_thread_stat(i, &t.pid, &t.ticks, t.name);
        if (st < 0) break;
        if (st == 0) continue;
        t.name[15] = 0;
        t.state = st;
        total += t.ticks;
        if (i == 0) { idle = t.ticks; continue; }
        int j = nths++;
        while (j > 0 && ths[j - 1].pid > t.pid) { ths[j] = ths[j - 1]; j--; }
        ths[j] = t;
    }
    /* finished threads take their ticks with them: count elapsed time instead of the sum */
    uint64_t dt = now - last_sample, didle = idle - last_idle;
    if (!sampled || dt == 0) dt = 1;
    (void)total;
    memcpy(prev, procs, sizeof(struct prow) * (size_t)nprocs);
    nprev = nprocs;
    nprocs = 0;
    nthreads = nths;
    int8_t used[512] = { 0 };          /* ths[] slots that belong to a program */
    /* programs: one row per thread group */
    for (int i = 0; i < MAXTASK && nprocs < MAXP - 1; i++) {
        struct task *t = task_slot(i);
        if (!t->used || t->state == 2 || t->is_thread) continue;
        struct prow *r = &procs[nprocs++];
        memset(r, 0, sizeof *r);
        r->pid = t->tgid;
        strlcpy(r->name, t->name, sizeof r->name);
        strlcpy(r->exe, t->exe, sizeof r->exe);
        r->uid = t->ns.cred.uid;
        user_name(r->uid, r->user, sizeof r->user);
        uint64_t f;
        __asm__ volatile("pushfq; pop %0; cli" : "=r"(f) :: "memory");
        r->rss = mm_rss(t->mm);                     /* t->mm can't go away with interrupts off */
        if (f & 0x200) __asm__ volatile("sti");
        for (int k = 0; k < MAXTASK; k++) {
            struct task *o = task_slot(k);
            if (!o->used || o->tgid != t->tgid) continue;
            r->threads++;
            struct th *h = th_find(o->pid);
            if (h) { r->ticks += h->ticks; used[h - ths] = 1; }
        }
    }
    /* the desktop (Chrono) gets its own row: drawing the screen is most of the kernel's work */
    struct prow *dr = &procs[nprocs];
    memset(dr, 0, sizeof *dr);
    dr->kernel = 2; dr->pid = 0;
    strlcpy(dr->name, "Chrono desktop", sizeof dr->name);
    strlcpy(dr->user, "kernel", sizeof dr->user);
    strlcpy(dr->exe, "draws the screen, windows and the built-in apps", sizeof dr->exe);
    for (int i = 0; i < nths; i++) if (!used[i] && !strcmp(ths[i].name, "chrono")) { dr->ticks += ths[i].ticks; dr->threads++; used[i] = 1; }
    if (dr->threads) nprocs++;
    /* the kernel's own threads: one row */
    struct prow *kr = &procs[nprocs];
    memset(kr, 0, sizeof *kr);
    kr->kernel = 1; kr->pid = 0;
    strlcpy(kr->name, "AuxoBSD kernel", sizeof kr->name);
    strlcpy(kr->user, "kernel", sizeof kr->user);
    strlcpy(kr->exe, "desktop, drivers, network, disk", sizeof kr->exe);
    size_t ht, hu;
    rs_heap_stats(&ht, &hu);
    kr->rss = hu / PAGE_SIZE;
    for (int i = 0; i < nths; i++) if (!used[i]) { kr->ticks += ths[i].ticks; kr->threads++; }
    nprocs++;
    prog_pages = 0;
    for (int i = 0; i < nprocs; i++) {
        struct prow *r = &procs[i];
        if (!r->kernel) prog_pages += r->rss;
        uint64_t before = r->ticks;
        for (int k = 0; k < nprev; k++) if (prev[k].pid == r->pid && prev[k].kernel == r->kernel) { before = prev[k].ticks; break; }
        if (!sampled) before = r->ticks;
        uint64_t d = r->ticks >= before ? r->ticks - before : 0;
        r->cpu10 = (int)(d * 1000 / dt);
        if (r->cpu10 > 1000) r->cpu10 = 1000;
    }
    cpu_now = sampled ? 1000 - (int)(didle * 1000 / dt) : 0;
    if (cpu_now < 0) cpu_now = 0;
    if (cpu_now > 1000) cpu_now = 1000;
    uint64_t rx = net_rx_bytes, tx = net_tx_bytes;
    rx_rate = sampled ? (rx - last_rx) * AUXO_HZ / dt : 0;
    tx_rate = sampled ? (tx - last_tx) * AUXO_HZ / dt : 0;
    size_t tot = pmm_total_pages(), fr = pmm_free_pages();
    push(h_cpu, (cpu_now + 5) / 10);
    push(h_mem, tot ? (int)((tot - fr) * 100 / tot) : 0);
    push32(h_rx, (uint32_t)(rx_rate > 0xFFFFFFFFu ? 0xFFFFFFFFu : rx_rate));
    push32(h_tx, (uint32_t)(tx_rate > 0xFFFFFFFFu ? 0xFFFFFFFFu : tx_rate));
    if (hn < HIST) hn++;
    last_sample = now; last_idle = idle; last_total = total; last_rx = rx; last_tx = tx;
    sampled = 1;
}

int sysmon_tick(void)
{
    if (sampled && kernel_ticks() - last_sample < AUXO_HZ) return 0;
    sample();
    return 1;
}

/* ── formatting ─────────────────────────────────────────────────────── */
static void bytes_str(uint64_t b, char *o, size_t n)
{
    if (b < 1024) ksnprintf(o, n, "%lu B", (unsigned long)b);
    else if (b < 1024 * 1024) ksnprintf(o, n, "%lu.%lu KiB", (unsigned long)(b >> 10), (unsigned long)((b & 1023) * 10 >> 10));
    else if (b < 1024ULL << 20) ksnprintf(o, n, "%lu.%lu MiB", (unsigned long)(b >> 20), (unsigned long)((b & 0xFFFFF) * 10 >> 20));
    else ksnprintf(o, n, "%lu.%02lu GiB", (unsigned long)(b >> 30), (unsigned long)((b & 0x3FFFFFFF) * 100 >> 30));
}

static void pct_str(int tenths, char *o, size_t n) { ksnprintf(o, n, "%d.%d%%", tenths / 10, tenths % 10); }

/* ── processes tab state ────────────────────────────────────────────── */
enum { COL_NAME, COL_USER, COL_PID, COL_CPU, COL_MEM };
static int sort_col = COL_CPU, sort_desc = 1, sel_pid = -1, sel_kernel, pscroll, sscroll;
static char pstatus[160];
static uint64_t pstatus_at;
static int order[MAXP];

struct rect { int x, y, w, h; };
static struct rect r_tab[3], r_col[5], r_end, r_kill, r_list;
static int in(const struct rect *r, int x, int y) { return x >= r->x && y >= r->y && x < r->x + r->w && y < r->y + r->h; }

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int name_cmp(const char *a, const char *b)
{
    while (*a && lower(*a) == lower(*b)) { a++; b++; }
    return lower(*a) - lower(*b);
}

static int row_cmp(const struct prow *a, const struct prow *b)
{
    long d = 0;
    switch (sort_col) {
    case COL_NAME: d = name_cmp(a->name, b->name); break;
    case COL_USER: d = name_cmp(a->user, b->user); break;
    case COL_PID:  d = a->pid - b->pid; break;
    case COL_CPU:  d = a->cpu10 - b->cpu10; if (!d) d = (long)(a->ticks > b->ticks) - (long)(a->ticks < b->ticks); break;
    case COL_MEM:  d = (long)(a->rss > b->rss) - (long)(a->rss < b->rss); break;
    }
    if (!d) d = a->pid - b->pid;
    return (int)(sort_desc ? -d : d);
}

static void sort_rows(void)
{
    for (int i = 0; i < nprocs; i++) {
        int j = i;
        while (j > 0 && row_cmp(&procs[order[j - 1]], &procs[i]) > 0) { order[j] = order[j - 1]; j--; }
        order[j] = i;
    }
}

static struct prow *selected(void)
{
    for (int i = 0; i < nprocs; i++) if (procs[i].pid == sel_pid && procs[i].kernel == sel_kernel) return &procs[i];
    return 0;
}

static void signal_selected(int sig)
{
    struct prow *r = selected();
    pstatus_at = kernel_ticks();
    if (!r) { strlcpy(pstatus, "Pick a program first", sizeof pstatus); return; }
    if (r->kernel) { strlcpy(pstatus, r->kernel == 2 ? "The desktop is part of the kernel: it can't be ended" : "The kernel can't be ended: it's what runs everything else", sizeof pstatus); return; }
    struct task *t = task_by_pid(r->pid);
    if (!t || t->state == 2) { ksnprintf(pstatus, sizeof pstatus, "%s has already finished", r->name); return; }
    if (session_uid() != 0 && t->ns.cred.uid != session_uid() && t->ns.cred.euid != session_uid()) {
        ksnprintf(pstatus, sizeof pstatus, "%s belongs to %s: you can't end it (run kill with sudo in a terminal)", r->name, r->user);
        return;
    }
    t->pending |= 1ULL << (sig - 1);
    ksnprintf(pstatus, sizeof pstatus, sig == 9 ? "Killed %s (%d)" : "Asked %s (%d) to quit", r->name, r->pid);
}

/* ── drawing helpers ────────────────────────────────────────────────── */
static uint32_t dimmer(const struct app_colors *k, float t) { return gfx_mix(k->fg, k->bg, t) | 0xff000000; }
static uint32_t card_bg(const struct app_colors *k) { return gfx_shade(k->bg, k->light ? -0.035f : 0.05f) | 0xff000000; }

static void button(struct canvas *c, const struct app_ctx *a, struct rect *r, int x, int y, int w, const char *label, int on, uint32_t col)
{
    const struct app_colors *k = a->col;
    *r = (struct rect){ x - a->x, y - a->y, w, 28 };
    gfx_round(c, x, y, w, 28, 6, on ? col : k->field, 0);
    gfx_text(c, &font_ui, x + (w - gfx_text_width(&font_ui, label)) / 2, y + 6, label, on ? k->accent_fg : k->fg);
}

static void bar(struct canvas *c, int x, int y, int w, int h, int permille, uint32_t col, uint32_t track)
{
    gfx_round(c, x, y, w, h, h / 2, track, 0);
    int fw = (int)((long)w * permille / 1000);
    if (fw > 0) gfx_round(c, x, y, fw < h ? h : fw, h, h / 2, col, 0);
}

/* an area graph of v[0..n) scaled to max, newest on the right */
static void graph8(struct canvas *c, int x, int y, int w, int h, const uint8_t *v, int n, uint32_t col, uint32_t grid)
{
    for (int g = 1; g < 4; g++) gfx_fill(c, x, y + h * g / 4, w, 1, grid);
    if (n < 2) return;
    float step = (float)w / (HIST - 1);
    float x0 = (float)(x + w) - step * (float)(n - 1);
    float px = 0, py = 0;
    for (int i = 0; i < n; i++) {
        float fx = x0 + step * (float)i, fy = (float)(y + h) - (float)h * (float)v[i] / 100.0f;
        if (i) {
            for (int cx = (int)px; cx < (int)fx; cx++) {
                float t = (fx > px) ? ((float)cx - px) / (fx - px) : 0;
                gfx_column(c, cx, py + (fy - py) * t, y + h, with_alpha(col, 60));
            }
            gfx_line(c, px, py, fx, fy, 2, col);
        }
        px = fx; py = fy;
    }
}

static void graph32(struct canvas *c, int x, int y, int w, int h, const uint32_t *v, int n, uint32_t max, uint32_t col)
{
    uint8_t p[HIST];
    for (int i = 0; i < n; i++) p[i] = (uint8_t)(max ? (uint64_t)v[i] * 100 / max : 0);
    for (int i = 0; i < n; i++) if (p[i] > 100) p[i] = 100;
    float step = (float)w / (HIST - 1);
    float x0 = (float)(x + w) - step * (float)(n - 1), px = 0, py = 0;
    for (int i = 0; i < n; i++) {
        float fx = x0 + step * (float)i, fy = (float)(y + h) - (float)h * (float)p[i] / 100.0f;
        if (i) {
            for (int cx = (int)px; cx < (int)fx; cx++) {
                float t = (fx > px) ? ((float)cx - px) / (fx - px) : 0;
                gfx_column(c, cx, py + (fy - py) * t, y + h, with_alpha(col, 50));
            }
            gfx_line(c, px, py, fx, fy, 2, col);
        }
        px = fx; py = fy;
    }
}

/* ── the hardware ───────────────────────────────────────────────────── */
static int cpu_logical(void)
{
    uint32_t r[4];
    cpuid(0, r);
    if (r[0] >= 0xB) {
        uint32_t s[4];
        __asm__ volatile("cpuid" : "=a"(s[0]), "=b"(s[1]), "=c"(s[2]), "=d"(s[3]) : "a"(0xB), "c"(1));
        if ((s[1] & 0xFFFF) > 0) return (int)(s[1] & 0xFFFF);
    }
    cpuid(1, r);
    if (r[3] & (1u << 28)) { int n = (int)((r[1] >> 16) & 0xFF); if (n > 0) return n; }
    return 1;
}

static void cpu_vendor(char *o)
{
    uint32_t r[4];
    cpuid(0, r);
    memcpy(o, &r[1], 4); memcpy(o + 4, &r[3], 4); memcpy(o + 8, &r[2], 4); o[12] = 0;
}

static int cpu_mhz(void)
{
    uint32_t r[4];
    cpuid(0, r);
    if (r[0] >= 0x16) { cpuid(0x16, r); if (r[0] & 0xFFFF) return (int)(r[0] & 0xFFFF); }
    /* "… @ 3.60GHz" in the full brand string */
    char raw[49];
    cpuid(0x80000000, r);
    if (r[0] < 0x80000004) return 0;
    for (uint32_t i = 0; i < 3; i++) { cpuid(0x80000002 + i, r); memcpy(raw + i * 16, r, 16); }
    raw[48] = 0;
    char *at = strchr(raw, '@');
    if (!at) return 0;
    at++;
    while (*at == ' ') at++;
    int whole = 0, frac = 0, fd = 0;
    while (*at >= '0' && *at <= '9') whole = whole * 10 + (*at++ - '0');
    if (*at == '.') { at++; while (*at >= '0' && *at <= '9' && fd < 3) { frac = frac * 10 + (*at++ - '0'); fd++; } }
    while (fd < 3) { frac *= 10; fd++; }
    return whole * 1000 + frac;
}

static void cpu_features(char *o, size_t n)
{
    uint32_t r1[4], r7[4] = { 0 }, rx[4] = { 0 }, r0[4];
    cpuid(0, r0); cpuid(1, r1);
    if (r0[0] >= 7) cpuid(7, r7);
    cpuid(0x80000001, rx);
    o[0] = 0;
    struct { int on; const char *name; } f[] = {
        { (int)(r1[3] >> 26) & 1, "SSE2" }, { (int)(r1[2] >> 0) & 1, "SSE3" }, { (int)(r1[2] >> 20) & 1, "SSE4.2" },
        { (int)(r1[2] >> 28) & 1, "AVX" }, { (int)(r7[1] >> 5) & 1, "AVX2" }, { (int)(r7[1] >> 16) & 1, "AVX-512" },
        { (int)(r1[2] >> 25) & 1, "AES" }, { (int)(r7[1] >> 29) & 1, "SHA" }, { (int)(r1[2] >> 12) & 1, "FMA" },
        { (int)(r1[2] >> 5) & 1, "VT-x" }, { (int)(rx[2] >> 2) & 1, "AMD-V" }, { (int)(r1[2] >> 31) & 1, "(virtual machine)" },
    };
    size_t o_n = 0;
    for (size_t i = 0; i < sizeof f / sizeof *f; i++) {
        if (!f[i].on) continue;
        o_n += (size_t)ksnprintf(o + o_n, n - o_n, "%s%s", o_n ? " · " : "", f[i].name);
        if (o_n >= n) break;
    }
}

/* ── tabs ───────────────────────────────────────────────────────────── */
static void paint_processes(const struct app_ctx *a, int top)
{
    struct canvas *c = a->c;
    const struct app_colors *k = a->col;
    int X = a->x, W = a->w, Y = top, H = a->y + a->h - top;
    static const char *const HEAD[] = { "Name", "User", "PID", "CPU", "Memory" };
    int cx[5] = { X + 16, X + W - 470, X + W - 340, X + W - 250, X + W - 140 };
    if (cx[1] < X + 220) { cx[1] = X + 220; }
    for (int i = 0; i < 5; i++) {
        char h[24];
        ksnprintf(h, sizeof h, "%s%s", HEAD[i], sort_col == i ? (sort_desc ? " ▾" : " ▴") : "");
        gfx_text(c, &font_bold, cx[i], Y + 8, h, sort_col == i ? k->fg : k->dim);
        int x1 = i < 4 ? cx[i + 1] : X + W;
        r_col[i] = (struct rect){ cx[i] - a->x - 6, Y - a->y, x1 - cx[i], 30 };
    }
    gfx_fill(c, X, Y + 30, W, 1, dimmer(k, 0.85f));
    int ly = Y + 32, lh = H - 32 - 44;
    r_list = (struct rect){ 0, ly - a->y, W, lh };
    sort_rows();
    int vis = lh / ROW;
    if (pscroll > nprocs - vis) pscroll = nprocs - vis;
    if (pscroll < 0) pscroll = 0;
    gfx_clip(c, X, ly, W, lh);
    for (int i = pscroll; i < nprocs && i < pscroll + vis + 1; i++) {
        struct prow *r = &procs[order[i]];
        int ry = ly + (i - pscroll) * ROW;
        int is_sel = r->pid == sel_pid && r->kernel == sel_kernel;
        if (is_sel) gfx_round(c, X + 6, ry + 1, W - 12, ROW - 2, 6, k->sel, 0);
        else if (i & 1) gfx_fill(c, X, ry, W, ROW, with_alpha(k->fg, 8));
        /* a little dot per program: green when busy */
        gfx_circle(c, (float)(cx[0] + 4), (float)(ry + ROW / 2), 4, r->kernel ? k->accent : r->cpu10 >= 50 ? 0xff3fb950 : dimmer(k, 0.6f));
        char nm[64];
        if (r->threads > 1) ksnprintf(nm, sizeof nm, "%s  (%d threads)", r->name, r->threads);
        else strlcpy(nm, r->name, sizeof nm);
        gfx_text_fit(c, &font_ui, cx[0] + 16, ry + 5, cx[1] - cx[0] - 24, nm, k->fg);
        gfx_text_fit(c, &font_ui, cx[1], ry + 5, cx[2] - cx[1] - 8, r->user, r->uid == 0 && !r->kernel ? k->danger : k->dim);
        char s[24];
        if (r->kernel) strlcpy(s, "—", sizeof s); else ksnprintf(s, sizeof s, "%d", r->pid);
        gfx_text(c, &font_ui, cx[2], ry + 5, s, k->dim);
        pct_str(r->cpu10, s, sizeof s);
        gfx_text(c, &font_ui, cx[3], ry + 5, s, r->cpu10 >= 500 ? k->danger : k->fg);
        if (r->kernel == 2) strlcpy(s, "in kernel", sizeof s); else bytes_str(r->rss * PAGE_SIZE, s, sizeof s);
        gfx_text(c, &font_ui, cx[4], ry + 5, s, r->kernel == 2 ? k->dim : k->fg);
    }
    gfx_noclip(c);
    /* bottom bar */
    int by = a->y + a->h - 44;
    gfx_fill(c, X, by, W, 44, k->field);
    struct prow *s = selected();
    char line[220];
    if (pstatus[0] && kernel_ticks() - pstatus_at < 6 * AUXO_HZ) strlcpy(line, pstatus, sizeof line);
    else if (s) ksnprintf(line, sizeof line, "%s — %s", s->name, s->exe[0] ? s->exe : "");
    else {
        int np = 0;
        for (int i = 0; i < nprocs; i++) if (!procs[i].kernel) np++;
        ksnprintf(line, sizeof line, "%d program%s · %d threads · click a column to sort", np, np == 1 ? "" : "s", nthreads);
    }
    gfx_text_fit(c, &font_ui, X + 14, by + 13, W - 280, line, k->dim);
    int can = s && !s->kernel;
    button(c, a, &r_end, X + W - 250, by + 8, 120, "End process", can, k->accent);
    button(c, a, &r_kill, X + W - 122, by + 8, 108, "Kill", can, k->danger);
}

static void card(struct canvas *c, const struct app_colors *k, int x, int y, int w, int h, const char *title, const char *big, uint32_t col)
{
    gfx_round(c, x, y, w, h, 10, card_bg(k), 0);
    gfx_fill(c, x + 14, y + 16, 10, 10, col);
    gfx_text(c, &font_bold, x + 32, y + 12, title, k->fg);
    if (big) gfx_text(c, &font_bold, x + w - 14 - gfx_text_width(&font_bold, big), y + 12, big, col);
}

static void paint_resources(const struct app_ctx *a, int top)
{
    struct canvas *c = a->c;
    const struct app_colors *k = a->col;
    int X = a->x + 12, W = a->w - 24, Y = top + 6, H = a->y + a->h - Y - 12;
    int cw = (W - 12) / 2, ch = (H - 12) / 2;
    uint32_t grid = dimmer(k, 0.88f), track = dimmer(k, 0.85f);
    uint32_t c_cpu = 0xff3b82f6, c_mem = 0xffa855f7, c_rx = 0xff22c55e, c_tx = 0xfff59e0b, c_dsk = 0xff06b6d4;
    char s[64], t[64], u[64];
    /* CPU */
    pct_str(cpu_now, s, sizeof s);
    card(c, k, X, Y, cw, ch, "Processor", s, c_cpu);
    int gx = X + 14, gw = cw - 28, gy = Y + 44, gh = ch - 44 - 42;
    graph8(c, gx, gy, gw, gh, h_cpu, hn, c_cpu, grid);
    gfx_text_fit(c, &font_ui, gx, Y + ch - 34, gw, cpu_brand(), k->dim);
    ksnprintf(t, sizeof t, "1 core in use of %d · last two minutes", cpu_logical());
    gfx_text_fit(c, &font_ui, gx, Y + ch - 18, gw, t, dimmer(k, 0.5f));
    /* Memory */
    size_t tot = pmm_total_pages(), fr = pmm_free_pages(), usedp = tot - fr;
    size_t ht, hu;
    rs_heap_stats(&ht, &hu);
    bytes_str((uint64_t)usedp * PAGE_SIZE, s, sizeof s);
    bytes_str((uint64_t)tot * PAGE_SIZE, t, sizeof t);
    ksnprintf(u, sizeof u, "%s of %s", s, t);
    int mx = X + cw + 12;
    card(c, k, mx, Y, cw, ch, "Memory", u, c_mem);
    graph8(c, mx + 14, gy, gw, gh - 22, h_mem, hn, c_mem, grid);
    /* what the used memory is: programs, the kernel's heap, files and the rest */
    uint64_t prog = prog_pages < usedp ? prog_pages : usedp;
    uint64_t heap = hu / PAGE_SIZE; if (prog + heap > usedp) heap = usedp - prog;
    uint64_t other = usedp - prog - heap;
    int by = Y + ch - 52, bw = gw;
    gfx_round(c, mx + 14, by, bw, 10, 5, track, 0);
    int w1 = tot ? (int)((uint64_t)bw * prog / tot) : 0, w2 = tot ? (int)((uint64_t)bw * heap / tot) : 0, w3 = tot ? (int)((uint64_t)bw * other / tot) : 0;
    gfx_fill(c, mx + 14, by, w1, 10, c_mem);
    gfx_fill(c, mx + 14 + w1, by, w2, 10, gfx_shade(c_mem, -0.35f) | 0xff000000);
    gfx_fill(c, mx + 14 + w1 + w2, by, w3, 10, gfx_mix(c_mem, k->bg, 0.55f) | 0xff000000);
    char l1[32], l2[32], l3[32], l4[32], line[200];
    bytes_str(prog * PAGE_SIZE, l1, sizeof l1); bytes_str(heap * PAGE_SIZE, l2, sizeof l2);
    bytes_str(other * PAGE_SIZE, l3, sizeof l3); bytes_str((uint64_t)fr * PAGE_SIZE, l4, sizeof l4);
    const char *lab[4] = { "Programs", "Kernel", auxofs_mounted() ? "Files & cache" : "Files (live)", "Free" };
    const char *val[4] = { l1, l2, l3, l4 };
    uint32_t chip[4] = { c_mem, gfx_shade(c_mem, -0.35f) | 0xff000000, gfx_mix(c_mem, k->bg, 0.55f) | 0xff000000, track };
    for (int i = 0; i < 4; i++) {
        int lx = mx + 14 + (i & 1) * (gw / 2), lyy = Y + ch - 36 + (i >> 1) * 18;
        gfx_round(c, lx, lyy + 4, 9, 9, 2, chip[i], 0);
        ksnprintf(line, sizeof line, "%s  %s", lab[i], val[i]);
        gfx_text_fit(c, &font_ui, lx + 15, lyy, gw / 2 - 20, line, i == 3 ? dimmer(k, 0.45f) : k->dim);
    }
    /* Network */
    int ny = Y + ch + 12;
    char rr[24], tr[24];
    bytes_str(rx_rate, rr, sizeof rr); bytes_str(tx_rate, tr, sizeof tr);
    ksnprintf(u, sizeof u, "↓ %s/s  ↑ %s/s", rr, tr);
    card(c, k, X, ny, cw, ch, "Network", u, c_rx);
    uint32_t mxr = 1024;
    for (int i = 0; i < hn; i++) { if (h_rx[i] > mxr) mxr = h_rx[i]; if (h_tx[i] > mxr) mxr = h_tx[i]; }
    for (int g = 1; g < 4; g++) gfx_fill(c, gx, ny + 44 + gh * g / 4, gw, 1, grid);
    graph32(c, gx, ny + 44, gw, gh, h_rx, hn, mxr, c_rx);
    graph32(c, gx, ny + 44, gw, gh, h_tx, hn, mxr, c_tx);
    bytes_str(net_rx_bytes, rr, sizeof rr); bytes_str(net_tx_bytes, tr, sizeof tr);
    ksnprintf(line, sizeof line, "Received %s · Sent %s since start-up", rr, tr);
    gfx_text_fit(c, &font_ui, gx, ny + ch - 34, gw, line, k->dim);
    gfx_text_fit(c, &font_ui, gx, ny + ch - 18, gw, net_present() ? net_status() : "No network card found", dimmer(k, 0.5f));
    /* Storage */
    card(c, k, mx, ny, cw, ch, "Storage", 0, c_dsk);
    int sy = ny + 46;
    if (auxofs_mounted()) {
        uint64_t T = auxofs_total_blocks() * 4096ULL, F = auxofs_free_blocks() * 4096ULL;
        bytes_str(T - F, s, sizeof s); bytes_str(T, t, sizeof t);
        ksnprintf(line, sizeof line, "System (%s, AuxoFS): %s of %s used", auxofs_disk(), s, t);
        gfx_text_fit(c, &font_ui, mx + 14, sy, gw, line, k->fg);
        bar(c, mx + 14, sy + 22, gw, 10, T ? (int)((T - F) * 1000 / T) : 0, c_dsk, track);
        sy += 44;
    } else {
        gfx_text_fit(c, &font_ui, mx + 14, sy, gw, "Live session: nothing is saved to a disk", k->fg);
        sy += 28;
    }
    for (int i = 0; i < disk_count() && sy < ny + ch - 26; i++) {
        struct disk *d = disk_get(i);
        if (!d || d->gone) continue;
        bytes_str(d->sectors * 512, s, sizeof s);
        ksnprintf(line, sizeof line, "%s · %s · %s%s%s", d->name, d->model[0] ? d->model : "disk", s, d->removable ? " · removable" : "", d->boot ? " · AuxoBSD is here" : "");
        gfx_text_fit(c, &font_ui, mx + 14, sy, gw, line, k->dim);
        sy += 20;
    }
    if (!disk_count()) gfx_text(c, &font_ui, mx + 14, sy, "No disks found", k->dim);
}

/* System tab: label/value rows in sections, scrollable */
static int sys_y, sys_top, sys_bottom;
static const struct app_ctx *sys_a;

static void sys_head(const char *title)
{
    struct canvas *c = sys_a->c;
    sys_y += 10;
    if (sys_y + 24 > sys_top && sys_y < sys_bottom) {
        gfx_text(c, &font_bold, sys_a->x + 24, sys_y, title, sys_a->col->accent);
        gfx_fill(c, sys_a->x + 24, sys_y + 22, sys_a->w - 48, 1, dimmer(sys_a->col, 0.85f));
    }
    sys_y += 30;
}

static void sys_row(const char *label, const char *value)
{
    struct canvas *c = sys_a->c;
    if (sys_y + 20 > sys_top && sys_y < sys_bottom) {
        gfx_text(c, &font_ui, sys_a->x + 24, sys_y, label, sys_a->col->dim);
        gfx_text_fit(c, &font_ui, sys_a->x + 200, sys_y, sys_a->w - 224, value, sys_a->col->fg);
    }
    sys_y += 22;
}

static int sys_height;

static void paint_system(const struct app_ctx *a, int top)
{
    struct canvas *c = a->c;
    char s[160], t[64];
    sys_a = a; sys_top = top; sys_bottom = a->y + a->h;
    int vis = sys_bottom - top;
    if (sscroll > sys_height - vis) sscroll = sys_height - vis;
    if (sscroll < 0) sscroll = 0;
    sys_y = top + 4 - sscroll;
    gfx_clip(c, a->x, top, a->w, vis);

    sys_head("This computer");
    sys_row("Name", auxo.hostname);
    sys_row("Signed in as", session_user());
    sys_row("System", "AuxoBSD " AUXO_VERSION " · x86_64 · runs Linux programs");
    fmt_uptime(t, sizeof t);
    sys_row("Up for", t);
    ksnprintf(s, sizeof s, "%s%s", boot_loader()[0] ? boot_loader() : "unknown loader", auxofs_mounted() ? " · installed on this computer" : " · live session");
    sys_row("Started by", s);

    sys_head("Processor");
    sys_row("Model", cpu_brand());
    cpu_vendor(t);
    sys_row("Maker", !strcmp(t, "GenuineIntel") ? "Intel" : !strcmp(t, "AuthenticAMD") ? "AMD" : t);
    int mhz = cpu_mhz();
    if (mhz) { ksnprintf(s, sizeof s, "%d.%02d GHz", mhz / 1000, mhz % 1000 / 10); sys_row("Speed", s); }
    ksnprintf(s, sizeof s, "%d (AuxoBSD uses 1 for now)", cpu_logical());
    sys_row("Threads", s);
    cpu_features(s, sizeof s);
    sys_row("Features", s);

    sys_head("Memory");
    bytes_str((uint64_t)pmm_total_pages() * PAGE_SIZE, t, sizeof t);
    sys_row("Installed", t);
    bytes_str((uint64_t)pmm_free_pages() * PAGE_SIZE, t, sizeof t);
    sys_row("Free now", t);
    sys_row("Swap", "none");

    sys_head("Graphics");
    const struct pci_dev *g = pci_gpu();
    if (g) { pci_describe(g, s, sizeof s); sys_row("Card", s); }
    else sys_row("Card", "not found on PCI");
    const struct fbinfo *fb = boot_framebuffer();
    if (fb) { ksnprintf(s, sizeof s, "%u × %u, %u-bit colour", fb->width, fb->height, fb->bpp); sys_row("Screen", s); }
    sys_row("Driver", "firmware framebuffer (no 3D acceleration)");

    sys_head("Storage");
    if (!disk_count()) sys_row("Disks", "none found");
    for (int i = 0; i < disk_count(); i++) {
        struct disk *d = disk_get(i);
        if (!d || d->gone) continue;
        bytes_str(d->sectors * 512, t, sizeof t);
        ksnprintf(s, sizeof s, "%s · %s · %s%s", d->model[0] ? d->model : "disk", d->bus, t, d->removable ? " · removable" : "");
        sys_row(d->name, s);
    }

    sys_head("Network");
    const struct nic *n = net_present() ? net_nic() : 0;
    if (n) {
        ksnprintf(s, sizeof s, "%s (%s)", n->model, n->driver);
        sys_row("Card", s);
        ksnprintf(s, sizeof s, "%02x:%02x:%02x:%02x:%02x:%02x", n->mac[0], n->mac[1], n->mac[2], n->mac[3], n->mac[4], n->mac[5]);
        sys_row("Hardware address", s);
        sys_row("Status", net_status());
        if (net_ready()) {
            char gw[20], dns[20];
            ip_fmt(net_gateway(), gw, sizeof gw); ip_fmt(net_dns(), dns, sizeof dns);
            (void)gw;
            sys_row("DNS server", dns);
        }
    } else sys_row("Card", "none that AuxoBSD can drive");
    if (pci_wifi()) { pci_describe(pci_wifi(), s, sizeof s); sys_row("Wi-Fi", s); }

    sys_head("Sound");
    const char *codec = hda_codec_name();
    if (codec && codec[0]) { ksnprintf(s, sizeof s, "HD Audio, codec %s · playing through %s", codec, hda_output_name()); sys_row("Card", s); }
    else sys_row("Card", "none found");

    sys_head("All devices");
    for (int i = 0; i < pci_count(); i++) {
        const struct pci_dev *d = pci_get(i);
        char nm[64], lab[24];
        pci_describe(d, nm, sizeof nm);
        ksnprintf(lab, sizeof lab, "%02x:%02x.%x", d->bus, d->dev, d->fn);
        ksnprintf(s, sizeof s, "%s — %s%s%s%s", pci_class_name(d->class, d->subclass), nm, d->driver ? " [" : "", d->driver ? d->driver : "", d->driver ? "]" : "");
        sys_row(lab, s);
    }
    sys_y += 12;
    sys_height = sys_y - (top + 4 - sscroll);
    gfx_noclip(c);
}

const char *sysmon_title(void) { return "System Monitor"; }

void sysmon_paint(const struct app_ctx *a)
{
    struct canvas *c = a->c;
    const struct app_colors *k = a->col;
    if (!sampled) sample();
    gfx_fill(c, a->x, a->y, a->w, a->h, k->bg);
    /* tabs, and the headline numbers on the right */
    static const char *const NAMES[] = { "Processes", "Resources", "System" };
    int tx = a->x + 12;
    for (int i = 0; i < 3; i++) {
        int w = gfx_text_width(&font_ui, NAMES[i]) + 28;
        button(c, a, &r_tab[i], tx, a->y + 12, w, NAMES[i], tab == i, k->accent);
        tx += w + 6;
    }
    char cpu[16], used[24], tot[24], line[96];
    pct_str(cpu_now, cpu, sizeof cpu);
    bytes_str((uint64_t)(pmm_total_pages() - pmm_free_pages()) * PAGE_SIZE, used, sizeof used);
    bytes_str((uint64_t)pmm_total_pages() * PAGE_SIZE, tot, sizeof tot);
    ksnprintf(line, sizeof line, "CPU %s   Memory %s / %s", cpu, used, tot);
    int lw = gfx_text_width(&font_ui, line);
    if (tx + 20 < a->x + a->w - lw - 16) gfx_text(c, &font_ui, a->x + a->w - lw - 16, a->y + 18, line, k->dim);
    gfx_fill(c, a->x, a->y + TABS, a->w, 1, dimmer(k, 0.85f));
    int top = a->y + TABS + 1;
    if (tab == TAB_PROC) paint_processes(a, top);
    else if (tab == TAB_RES) paint_resources(a, top);
    else paint_system(a, top);
}

int sysmon_click(int x, int y, int w, int h)
{
    (void)w; (void)h;
    for (int i = 0; i < 3; i++) if (in(&r_tab[i], x, y)) { tab = i; return 1; }
    if (tab != TAB_PROC) return 1;
    for (int i = 0; i < 5; i++) if (in(&r_col[i], x, y)) {
        if (sort_col == i) sort_desc = !sort_desc;
        else { sort_col = i; sort_desc = i == COL_CPU || i == COL_MEM; }
        return 1;
    }
    if (in(&r_end, x, y)) { signal_selected(15); return 1; }
    if (in(&r_kill, x, y)) { signal_selected(9); return 1; }
    if (in(&r_list, x, y)) {
        int i = pscroll + (y - r_list.y) / ROW;
        if (i >= 0 && i < nprocs) {
            struct prow *r = &procs[order[i]];
            sel_pid = r->pid; sel_kernel = r->kernel;
        } else sel_pid = -1;
    }
    return 1;
}

int sysmon_wheel(int dz)
{
    if (tab == TAB_PROC) pscroll += dz > 0 ? -3 : 3;
    else if (tab == TAB_SYS) sscroll += dz > 0 ? -66 : 66;
    if (pscroll < 0) pscroll = 0;
    if (sscroll < 0) sscroll = 0;
    return 1;
}

int sysmon_key(int k)
{
    if (k == '\t') { tab = (tab + 1) % 3; return 1; }
    if (tab == TAB_SYS) {
        if (k == KEY_DOWN) sscroll += 22; else if (k == KEY_UP) sscroll -= 22;
        else if (k == KEY_PGDN) sscroll += 300; else if (k == KEY_PGUP) sscroll -= 300;
        else if (k == KEY_HOME) sscroll = 0;
        if (sscroll < 0) sscroll = 0;
        return 1;
    }
    if (tab != TAB_PROC) return 0;
    sort_rows();
    int cur = -1;
    for (int i = 0; i < nprocs; i++) if (procs[order[i]].pid == sel_pid && procs[order[i]].kernel == sel_kernel) cur = i;
    if (k == KEY_DOWN || k == KEY_UP) {
        cur = k == KEY_DOWN ? cur + 1 : cur - 1;
        if (cur < 0) cur = 0;
        if (cur >= nprocs) cur = nprocs - 1;
        if (cur >= 0) { sel_pid = procs[order[cur]].pid; sel_kernel = procs[order[cur]].kernel; }
        if (cur < pscroll) pscroll = cur;
        return 1;
    }
    if (k == KEY_DEL) { signal_selected(15); return 1; }
    return 0;
}
