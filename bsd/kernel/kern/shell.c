/* auxsh: the AuxoBSD kernel shell. Runs as a kernel thread; commands are built in
 * until there is a userland. Line editing: Backspace, Ctrl-C, Ctrl-U, Ctrl-L,
 * Up/Down history, Tab completes command names. */
#include "kern.h"
#include "usb.h"
#include "arch.h"
#include "rs.h"
#include "auxo.h"
#include "chrono.h"
#include "pci.h"
#include "net.h"
#include "vfs.h"
#include "fs.h"
#include "uproc.h"
#include "disk.h"
#include "install.h"

#define LINE_MAX 1024
#define HIST     16
#define MAXARGS  96

struct cmd {
    const char *name;
    int (*fn)(int argc, char **argv);
    const char *help;
};

static const struct cmd cmds[];
static int cmd_strace(int argc, char **argv);

/* ── line editor ────────────────────────────────────────────────────── */
static char hist[HIST][LINE_MAX];
static int  nhist;

static void show_prompt(const char *prompt)
{
    if (prompt) { kprintf("%s", prompt); return; }
    c_rgb(auxo_c1()); kprintf("▲"); c_reset();
    kprintf(" \033[1mroot@%s\033[0m ", auxo.hostname);
    c_rgb(auxo_c1()); kprintf("❯"); c_reset();
    kprintf(" ");
}

static void erase(size_t n) { while (n--) kprintf("\b \b"); }

static void complete(char *buf, size_t *len, size_t cap)
{
    if (strchr(buf, ' ')) return;
    const struct cmd *hit = 0;
    int matches = 0;
    for (const struct cmd *c = cmds; c->name; c++)
        if (!strncmp(c->name, buf, *len)) { hit = c; matches++; }
    if (matches == 1) {
        size_t l = strlen(hit->name);
        for (size_t i = *len; i < l && i + 2 < cap; i++) { buf[i] = hit->name[i]; console_putc(hit->name[i]); }
        *len = l < cap - 2 ? l : cap - 2;
        buf[(*len)++] = ' ';
        console_putc(' ');
        buf[*len] = 0;
    } else if (matches > 1) {
        kprintf("\n");
        for (const struct cmd *c = cmds; c->name; c++)
            if (!strncmp(c->name, buf, *len)) kprintf("%s  ", c->name);
        kprintf("\n");
        buf[*len] = 0;
        show_prompt(0);
        kprintf("%s", buf);
    }
}

int shell_readline(const char *prompt, char *buf, size_t n)
{
    size_t len = 0;
    int h = nhist, esc = 0, last_cr = 0;
    buf[0] = 0;
    show_prompt(prompt);

    for (;;) {
        int c = input_getc();
        if (esc == 1) { esc = (c == '[') ? 2 : 0; continue; }
        if (esc == 2) {
            if (c >= '0' && c <= '9') continue;       /* e.g. ESC [ 3 ~ */
            esc = 0;
            if ((c == 'A' && h > 0) || (c == 'B' && h < nhist)) {
                h += (c == 'A') ? -1 : 1;
                erase(len);
                strlcpy(buf, h < nhist ? hist[h % HIST] : "", n);
                len = strlen(buf);
                kprintf("%s", buf);
            }
            continue;
        }
        if (c == 0x1b) { esc = 1; continue; }
        if (c == '\n' && last_cr) { last_cr = 0; continue; }   /* CR LF from a terminal */
        last_cr = (c == '\r');
        if (c == '\r' || c == '\n') { kprintf("\n"); buf[len] = 0; break; }
        if (c == 3) { kprintf("^C\n"); buf[0] = 0; return -1; }
        if (c == 0x15) { erase(len); len = 0; continue; }     /* Ctrl-U */
        if (c == 0x0c) { console_clear(); show_prompt(prompt); buf[len] = 0; kprintf("%s", buf); continue; }
        if (c == '\t') { buf[len] = 0; if (!prompt) complete(buf, &len, n); continue; }
        if (c == '\b' || c == 0x7f) { if (len) { len--; erase(1); } continue; }
        if (c < 0x20 || len + 1 >= n) continue;
        buf[len++] = (char)c;
        console_putc((char)c);
    }

    if (!prompt && len && (nhist == 0 || strcmp(hist[(nhist - 1) % HIST], buf))) {
        strlcpy(hist[nhist % HIST], buf, LINE_MAX);
        nhist++;
    }
    return (int)len;
}

static int split(char *s, char **argv)
{
    int argc = 0;
    while (*s && argc < MAXARGS - 1) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;
        if (*s == '"' || *s == '\'') {
            char q = *s++;
            argv[argc++] = s;
            while (*s && *s != q) s++;
        } else {
            argv[argc++] = s;
            while (*s && *s != ' ' && *s != '\t') s++;
        }
        if (*s) *s++ = 0;
    }
    argv[argc] = 0;
    return argc;
}

/* ── built-ins ──────────────────────────────────────────────────────── */
static int cmd_help(int argc, char **argv)
{
    (void)argc; (void)argv;
    c_bold(); kprintf("auxsh commands\n"); c_reset();
    for (const struct cmd *c = cmds; c->name; c++) {
        if (!c->help) continue;
        c_rgb(auxo_c1()); kprintf("  %-12s", c->name); c_reset();
        kprintf(" %s\n", c->help);
    }
    if (vfs_exists("/usr/bin/pkg")) {
        c_bold(); kprintf("software\n"); c_reset();
        c_rgb(auxo_c1()); kprintf("  %-12s", "pkg"); c_reset();
        kprintf(" install programs from Ubuntu 24.04: pkg update, pkg search WORD, pkg install NAME\n");
    }
    c_dim(); kprintf("Tab completes, Up/Down for history, Ctrl-L clears.\n"); c_reset();
    return 0;
}

static int cmd_clear(int argc, char **argv) { (void)argc; (void)argv; console_clear(); return 0; }

static int cmd_echo(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) kprintf("%s%s", argv[i], i + 1 < argc ? " " : "");
    kprintf("\n");
    return 0;
}

static int cmd_uname(int argc, char **argv)
{
    int all = argc > 1 && !strcmp(argv[1], "-a");
    if (argc == 1 || all || !strcmp(argv[1], "-s")) kprintf("%s", AUXO_NAME);
    if (all) kprintf(" %s", auxo.hostname);
    if (all || (argc > 1 && !strcmp(argv[1], "-r"))) kprintf("%s%s", all ? " " : "", AUXO_VERSION);
    if (all || (argc > 1 && !strcmp(argv[1], "-m"))) kprintf("%s%s", all ? " " : "", "x86_64");
    kprintf("\n");
    return 0;
}

static int cmd_uptime(int argc, char **argv)
{
    (void)argc; (void)argv;
    char up[32];
    fmt_uptime(up, sizeof up);
    kprintf("up %s, %lu threads, %lu ticks\n", up, (unsigned long)rs_proc_count(), (unsigned long)kernel_ticks());
    return 0;
}

static int cmd_free(int argc, char **argv)
{
    (void)argc; (void)argv;
    size_t t = pmm_total_pages() * PAGE_SIZE >> 10, f = pmm_free_pages() * PAGE_SIZE >> 10, ht, hu;
    rs_heap_stats(&ht, &hu);
    kprintf("%-8s %10s %10s %10s\n", "", "total", "used", "free");
    kprintf("%-8s %7lu KiB %7lu KiB %7lu KiB\n", "Mem:", (unsigned long)t, (unsigned long)(t - f), (unsigned long)f);
    kprintf("%-8s %7lu KiB %7lu KiB %7lu KiB\n", "Heap:", (unsigned long)(ht >> 10), (unsigned long)(hu >> 10),
            (unsigned long)((ht - hu) >> 10));
    return 0;
}

static int cmd_ps(int argc, char **argv) { (void)argc; (void)argv; rs_ps(); return 0; }

static int cmd_demo(int argc, char **argv)
{
    (void)argc; (void)argv;
    demo_start();
    for (uint64_t until = kernel_ticks() + 150; kernel_ticks() < until; ) __asm__ volatile("sti; hlt");
    return 0;
}

static int cmd_cmdline(int argc, char **argv)
{
    (void)argc; (void)argv;
    kprintf("%s\n", boot_cmdline()[0] ? boot_cmdline() : "(empty)");
    return 0;
}

static int cmd_history(int argc, char **argv)
{
    (void)argc; (void)argv;
    int from = nhist > HIST ? nhist - HIST : 0;
    for (int i = from; i < nhist; i++) kprintf("%4d  %s\n", i + 1, hist[i % HIST]);
    return 0;
}

static int cmd_lsusb(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (!usb_controllers()) { kprintf("no USB 3 (xHCI) controller found\n"); return 1; }
    static char buf[4096];
    if (!usb_list(buf, sizeof buf)) kprintf("no USB devices (yet: they show up a second after they're plugged in)\n");
    else kprintf("%s", buf);
    return 0;
}

static int cmd_lsblk(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (!disk_count()) { kprintf("no disks found (IDE, SATA/AHCI, NVMe and USB are supported)\n"); return 0; }
    for (int i = 0; i < disk_count(); i++) {
        struct disk *d = disk_get(i);
        if (d->gone || !d->sectors) continue;                  /* unplugged */
        uint64_t mb = d->sectors / 2048;
        kprintf("%-7s %-5s %6lu %s  %s\n", d->name, d->bus, (unsigned long)(mb >= 10240 ? mb / 1024 : mb), mb >= 10240 ? "GB" : "MB", d->model);
    }
    return 0;
}

/* disktest NAME: write a pattern near the end of the disk, read it back (destroys those sectors) */
static int cmd_disktest(int argc, char **argv)
{
    if (argc < 2) { kprintf("usage: disktest DISK   (overwrites 64 sectors near its end)\n"); return 1; }
    struct disk *d = 0;
    for (int i = 0; i < disk_count(); i++) if (!strcmp(disk_get(i)->name, argv[1]) && !disk_get(i)->gone) d = disk_get(i);
    if (!d) { kprintf("disktest: no disk %s\n", argv[1]); return 1; }
    uint8_t *a = (uint8_t *)(uintptr_t)pmm_alloc_contig(8), *b = (uint8_t *)(uintptr_t)pmm_alloc_contig(8);
    if (!a || !b) return 1;
    for (int i = 0; i < 32768; i++) a[i] = (uint8_t)(i * 7 + 13);
    uint64_t lba = d->sectors - 100;
    int w = d->write(d, lba, 64, a), r = d->read(d, lba, 64, b);
    int same = !memcmp(a, b, 32768);
    kprintf("%s: write %s, read %s, data %s\n", d->name, w ? "failed" : "ok", r ? "failed" : "ok", same ? "matches" : "DIFFERS");
    for (int i = 0; i < 8; i++) { pmm_free_page((uint64_t)(uintptr_t)a + (uint64_t)i * 4096); pmm_free_page((uint64_t)(uintptr_t)b + (uint64_t)i * 4096); }
    return !same;
}

static int cmd_sync(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (!store_where()) { kprintf("sync: this is a live system: files are only in memory (install AuxoBSD to keep them)\n"); return 1; }
    int r = store_save();
    kprintf(r == 0 ? "saved /home/root to %s\n" : "sync: couldn't write to %s\n", store_where());
    return r != 0;
}

/* install DISK [HOSTNAME]: the installer, from the shell */
static int cmd_install(int argc, char **argv)
{
    if (argc < 2) { kprintf("usage: install DISK [HOSTNAME] [--yes]   (see lsblk; the disk is erased)\n"); return 1; }
    int yes = !strcmp(argv[argc - 1], "--yes");
    if (yes) argc--;
    int idx = -1;
    for (int i = 0; i < disk_count(); i++) if (!strcmp(disk_get(i)->name, argv[1]) && !disk_get(i)->gone) idx = i;
    if (idx < 0) { kprintf("install: no disk %s (see lsblk)\n", argv[1]); return 1; }
    struct disk *d = disk_get(idx);
    if (!yes) {
        char ans[16], q[160];
        ksnprintf(q, sizeof q, "Erase %s (%s, %lu MB) and install AuxoBSD? Type yes: ", d->name, d->model, (unsigned long)(d->sectors / 2048));
        shell_readline(q, ans, sizeof ans);
        if (strcmp(ans, "yes")) { kprintf("not installing\n"); return 1; }
    }
    struct install_req r = { idx, "auxobsd", auxo.accent };
    strlcpy(r.hostname, argc > 2 ? argv[2] : auxo.hostname, sizeof r.hostname);
    if (install_start(&r) < 0) { kprintf("install: already running\n"); return 1; }
    int last = -1;
    while (install_state == 1) {
        if (install_pct / 10 != last) { last = install_pct / 10; kprintf("  %3d%%  %s\n", install_pct, install_msg); }
        kdelay_ms(200);
    }
    kprintf("%s\n", install_msg);
    return install_state != 2;
}

static int cmd_lspci(int argc, char **argv)
{
    (void)argc; (void)argv;
    for (int i = 0; i < pci_count(); i++) {
        const struct pci_dev *d = pci_get(i);
        char nm[64];
        pci_describe(d, nm, sizeof nm);
        kprintf("%02x:%02x.%x %-16s %04x:%04x  %s%s%s", d->bus, d->dev, d->fn, pci_class_name(d->class, d->subclass),
                d->vendor, d->device, nm, d->driver ? "  [" : "", d->driver ? d->driver : "");
        if (d->driver) kprintf("]");
        kprintf("\n");
    }
    return 0;
}

static int cmd_ifconfig(int argc, char **argv)
{
    (void)argc; (void)argv;
    const struct nic *n = net_nic();
    if (!n) { kprintf("net: %s\n", net_status()); return 1; }
    kprintf("%s: %s, mac %02x:%02x:%02x:%02x:%02x:%02x, link %s\n", n->driver, n->model, n->mac[0], n->mac[1], n->mac[2],
            n->mac[3], n->mac[4], n->mac[5], n->link((struct nic *)n) ? "up" : "down");
    if (!net_ready()) { kprintf("  %s\n", net_status()); return 0; }
    char a[16], m[16], g[16], d[16];
    ip_fmt(net_ip(), a, sizeof a); ip_fmt(net_mask(), m, sizeof m); ip_fmt(net_gateway(), g, sizeof g); ip_fmt(net_dns(), d, sizeof d);
    kprintf("  inet %s netmask %s gateway %s dns %s\n", a, m, g, d);
    const struct pci_dev *w = pci_wifi();
    if (w) { char nm[64]; pci_describe(w, nm, sizeof nm); kprintf("  wifi: %s (no driver yet)\n", nm); }
    return 0;
}

static int cmd_ping(int argc, char **argv)
{
    if (argc < 2) { kprintf("usage: ping HOST [-c COUNT]\n"); return 1; }
    int count = 4;
    if (argc > 3 && !strcmp(argv[2], "-c")) count = katoi(argv[3]);
    if (net_wait_ready(5000) < 0) { kprintf("ping: no network (%s)\n", net_status()); return 1; }
    uint32_t ip;
    if (net_resolve(argv[1], &ip, 5000) < 0) { kprintf("ping: can't resolve %s\n", argv[1]); return 1; }
    char a[16];
    ip_fmt(ip, a, sizeof a);
    kprintf("PING %s (%s)\n", argv[1], a);
    int got = 0;
    for (int i = 0; i < count; i++) {
        int rtt = net_ping(ip, (uint16_t)(i + 1), 2000);
        if (rtt >= 0) { got++; kprintf("reply from %s: seq=%d time=%d ms\n", a, i + 1, rtt); }
        else kprintf("seq=%d timed out\n", i + 1);
        if (i + 1 < count) kdelay_ms(rtt >= 0 && rtt < 1000 ? 1000 - (uint32_t)rtt : 0);
    }
    kprintf("%d sent, %d received\n", count, got);
    return got ? 0 : 1;
}

/* get URL: fetch over HTTP/HTTPS and print the start of the body */
static int cmd_get(int argc, char **argv)
{
    if (argc < 2) { kprintf("usage: get URL [-n BYTES]   (http:// or https://)\n"); return 1; }
    int show = argc > 3 && !strcmp(argv[2], "-n") ? katoi(argv[3]) : 600;
    if (net_wait_ready(8000) < 0) { kprintf("get: no network (%s)\n", net_status()); return 1; }
    size_t cap = 1 << 20;
    uint64_t pg = pmm_alloc_contig(cap / PAGE_SIZE);
    if (!pg) { kprintf("get: out of memory\n"); return 1; }
    char *buf = (char *)(uintptr_t)pg;
    static struct http_result r;
    char url[512];
    if (strncmp(argv[1], "http://", 7) && strncmp(argv[1], "https://", 8)) ksnprintf(url, sizeof url, "http://%s", argv[1]);
    else strlcpy(url, argv[1], sizeof url);
    int rc = http_get(url, buf, cap, &r, 0);
    if (rc < 0) kprintf("get: %s\n", r.error);
    else {
        kprintf("HTTP %d, %lu bytes, %s\n%s\n", r.status, (unsigned long)r.len, r.content_type[0] ? r.content_type : "?", r.url);
        size_t n = r.len < (size_t)show ? r.len : (size_t)show;
        console_write(r.body, n);
        if (n < r.len) kprintf("\n… (%lu more bytes)\n", (unsigned long)(r.len - n));
        else kprintf("\n");
    }
    for (size_t i = 0; i < cap / PAGE_SIZE; i++) pmm_free_page(pg + i * PAGE_SIZE);
    return rc < 0;
}

/* files: the in-memory filesystem (saved to disk on an installed system) */
static char cwd[VFS_PATH] = "/home/root";

static void abspath(const char *p, char *out, size_t n)
{
    if (p[0] == '/') strlcpy(out, p, n);
    else if (!strcmp(p, "~")) strlcpy(out, "/home/root", n);
    else if (p[0] == '~' && p[1] == '/') ksnprintf(out, n, "/home/root/%s", p + 2);
    else ksnprintf(out, n, "%s/%s", cwd, p);
}

static int cmd_ls(int argc, char **argv)
{
    char dir[VFS_PATH];
    abspath(argc > 1 ? argv[1] : ".", dir, sizeof dir);
    size_t dl = strlen(dir);
    if (dl > 2 && !strcmp(dir + dl - 2, "/.")) dir[dl -= 2] = 0;
    int shown = 0;
    char last[VFS_PATH] = "";
    for (int i = 0; i < vfs_count(); i++) {
        const char *p = vfs_path(i);
        if (!strcmp(p, dir)) { kprintf("%8lu  %s\n", (unsigned long)vfs_size(i), p); return 0; }
        if (strncmp(p, dir, dl) || (dl > 1 && p[dl] != '/')) continue;
        const char *rest = p + (dl > 1 ? dl + 1 : 1);
        const char *slash = strchr(rest, '/');
        if (slash) {                                         /* a subdirectory: show once */
            char sub[VFS_PATH]; size_t sl = (size_t)(slash - rest);
            memcpy(sub, rest, sl); sub[sl] = 0;
            if (!strcmp(sub, last)) continue;
            strlcpy(last, sub, sizeof last);
            c_rgb(auxo_c1()); kprintf("%8s  %s/\n", "dir", sub); c_reset();
        } else kprintf("%8lu  %s\n", (unsigned long)vfs_size(i), rest);
        shown++;
    }
    if (!shown) kprintf("(empty)\n");
    return 0;
}

static int cmd_cat(int argc, char **argv)
{
    if (argc < 2) { kprintf("usage: cat FILE\n"); return 1; }
    char p[VFS_PATH]; const char *d; size_t n;
    abspath(argv[1], p, sizeof p);
    if (vfs_read(p, &d, &n) < 0) { kprintf("cat: %s: no such file\n", argv[1]); return 1; }
    console_write(d, n);
    if (n && d[n - 1] != '\n') kprintf("\n");
    return 0;
}

static int cmd_rm(int argc, char **argv)
{
    if (argc < 2) { kprintf("usage: rm FILE\n"); return 1; }
    char p[VFS_PATH];
    abspath(argv[1], p, sizeof p);
    if (vfs_remove(p) < 0) { kprintf("rm: %s: no such file\n", argv[1]); return 1; }
    return 0;
}

static int cmd_cp(int argc, char **argv)
{
    if (argc < 3) { kprintf("usage: cp FROM TO\n"); return 1; }
    char a[VFS_PATH], b[VFS_PATH]; const char *d; size_t n;
    abspath(argv[1], a, sizeof a); abspath(argv[2], b, sizeof b);
    if (vfs_read(a, &d, &n) < 0) { kprintf("cp: %s: no such file\n", argv[1]); return 1; }
    return vfs_write(b, d, n) < 0;
}

static int cmd_mv(int argc, char **argv)
{
    if (cmd_cp(argc, argv)) return 1;
    char a[VFS_PATH]; abspath(argv[1], a, sizeof a);
    vfs_remove(a);
    return 0;
}

static int cmd_touch(int argc, char **argv)
{
    if (argc < 2) { kprintf("usage: touch FILE\n"); return 1; }
    char p[VFS_PATH]; abspath(argv[1], p, sizeof p);
    if (!vfs_exists(p)) vfs_write(p, "", 0);
    return 0;
}

static int cmd_pwd(int argc, char **argv) { (void)argc; (void)argv; kprintf("%s\n", cwd); return 0; }
static int cmd_cd(int argc, char **argv)
{
    char p[VFS_PATH];
    abspath(argc > 1 ? argv[1] : "~", p, sizeof p);
    size_t l = strlen(p);
    while (l > 1 && p[l - 1] == '/') p[--l] = 0;
    strlcpy(cwd, p, sizeof cwd);
    return 0;
}

static int cmd_aether(int argc, char **argv)
{
    if (!chrono_running()) { kprintf("aether: needs the Chrono desktop (it's a graphical editor)\n"); return 1; }
    if (argc < 2) { chrono_request_open(APP_AETHER); return 0; }
    char p[VFS_PATH]; abspath(argv[1], p, sizeof p);
    chrono_request_edit(p);
    return 0;
}

static int cmd_browse(int argc, char **argv)
{
    if (!chrono_running()) { kprintf("browse: needs the Chrono desktop; try get URL\n"); return 1; }
    chrono_request_url(argc > 1 ? argv[1] : "about:home");
    return 0;
}

/* chrono: the desktop, from the shell. Only posts requests; the Chrono thread does the drawing. */
static int cmd_chrono(int argc, char **argv)
{
    static const char *const apps[APP_COUNT] = { "terminal", "settings", "about", "browser", "aether", "installer" };
    const char *sub = argc > 1 ? argv[1] : "status";
    if (!strcmp(sub, "themes")) {
        for (int i = 0; chrono_theme_id(i); i++) {
            int on = chrono_running() && i == chrono_current_theme();
            if (on) c_rgb(auxo_c1());
            kprintf("%s %-18s %-22s %s\n", on ? "●" : " ", chrono_theme_id(i), chrono_theme_name(i),
                    chrono_theme_retro(i) ? "retro" : "modern");
            if (on) c_reset();
        }
        return 0;
    }
    if (!strcmp(sub, "help") || !strcmp(sub, "--help")) {
        kprintf("usage: chrono [status]           desktop and theme\n"
                "       chrono themes             list the Chrono themes\n"
                "       chrono theme NAME         switch theme\n"
                "       chrono open APP           terminal, settings, about, browser, aether, installer\n");
        return 0;
    }
    if (!chrono_running()) {
        kprintf("chrono: the desktop isn't running (text mode). Boot the \"AuxoBSD\" GRUB entry for Chrono.\n");
        return !strcmp(sub, "status") ? 0 : 1;
    }
    if (!strcmp(sub, "status")) {
        kprintf("Chrono desktop on %dx%d, theme %s (%s)\n", fb_width(), fb_height(),
                chrono_theme_name(chrono_current_theme()), chrono_theme_id(chrono_current_theme()));
        return 0;
    }
    if (!strcmp(sub, "theme") && argc > 2) {
        int i = chrono_theme_find(argv[2]);
        if (i < 0) { kprintf("chrono: no theme '%s' (see chrono themes)\n", argv[2]); return 1; }
        chrono_request_theme(i);
        kprintf("theme → %s\n", chrono_theme_name(i));
        return 0;
    }
    if (!strcmp(sub, "open") && argc > 2) {
        for (int a = 0; a < APP_COUNT; a++)
            if (!strcmp(argv[2], apps[a])) { chrono_request_open(a); return 0; }
        kprintf("chrono: no app '%s' (terminal, settings, about, browser, aether, installer)\n", argv[2]);
        return 1;
    }
    kprintf("chrono: try chrono help\n");
    return 1;
}

static int cmd_reboot(int argc, char **argv) { (void)argc; (void)argv; kprintf("rebooting...\n"); machine_reboot(); }
static int cmd_poweroff(int argc, char **argv) { (void)argc; (void)argv; kprintf("powering off...\n"); machine_poweroff(); }

static const struct cmd cmds[] = {
    { "auxo-fetch",  cmd_auxo_fetch,  "system summary in your accent colour (--small, --no-logo, --json)" },
    { "auxo-tweak",  cmd_auxo_tweak,  "change accent, hostname, MOTD, colour (auxo-tweak help)" },
    { "auxo-motd",   cmd_auxo_motd,   "the Auxo greeting (--tip, --give, --off, --on)" },
    { "fetch",       cmd_auxo_fetch,  0 },
    { "help",        cmd_help,        "this list" },
    { "clear",       cmd_clear,       "clear the screen" },
    { "echo",        cmd_echo,        "print its arguments" },
    { "uname",       cmd_uname,       "system name (-a -s -r -m)" },
    { "uptime",      cmd_uptime,      "time since boot" },
    { "free",        cmd_free,        "physical memory and kernel heap" },
    { "ps",          cmd_ps,          "kernel threads" },
    { "ls",          cmd_ls,          "list files" },
    { "cat",         cmd_cat,         "print a file" },
    { "cp",          cmd_cp,          "copy a file" },
    { "mv",          cmd_mv,          "move / rename a file" },
    { "rm",          cmd_rm,          "delete a file" },
    { "touch",       cmd_touch,       "create an empty file" },
    { "cd",          cmd_cd,          "change directory" },
    { "pwd",         cmd_pwd,         "current directory" },
    { "edit",        cmd_aether,      "open a file in the desktop's built-in editor" },
    { "browse",      cmd_browse,      "open a page in the Browser" },
    { "strace",      cmd_strace,      "run a program and show its system calls" },
    { "install",     cmd_install,     "install AuxoBSD on a disk (or use the Install app)" },
    { "sync",        cmd_sync,        "save /home/root to the disk now (installed systems)" },
    { "lsblk",       cmd_lsblk,       "disks (IDE, SATA, NVMe)" },
    { "lsusb",       cmd_lsusb,       "USB devices (keyboards, mice, hubs)" },
    { "disktest",    cmd_disktest,    "write and read back a test pattern on a disk" },
    { "lspci",       cmd_lspci,       "PCI devices and their drivers" },
    { "ifconfig",    cmd_ifconfig,    "network card, address, gateway, DNS" },
    { "ping",        cmd_ping,        "ICMP echo: ping HOST [-c N]" },
    { "get",         cmd_get,         "fetch a web page: get URL (http or https)" },
    { "chrono",      cmd_chrono,      "the Chrono desktop: themes, theme NAME, open APP" },
    { "demo",        cmd_demo,        "run the scheduler / syscall demo threads" },
    { "cmdline",     cmd_cmdline,     "the kernel command line from the boot loader" },
    { "history",     cmd_history,     "recent commands" },
    { "reboot",      cmd_reboot,      "restart the machine" },
    { "poweroff",    cmd_poweroff,    "turn the machine off" },
    { "halt",        cmd_poweroff,    0 },
    { 0, 0, 0 },
};

extern int sys_trace;
static void run(char *line);
static int cmd_strace(int argc, char **argv)
{
    if (argc < 2) { kprintf("usage: strace PROGRAM [ARGS]   (every system call, on the console)\n"); return 1; }
    char buf[LINE_MAX] = "";
    size_t o = 0;
    for (int i = 1; i < argc && o < sizeof buf - 2; i++) o += (size_t)ksnprintf(buf + o, sizeof buf - o, "%s%s", i > 1 ? " " : "", argv[i]);
    sys_trace = 1;
    run(buf);
    sys_trace = 0;
    return 0;
}

static void run(char *line)
{
    char *argv[MAXARGS];
    int argc = split(line, argv);
    if (!argc) return;
    /* NAME=value before the command: extra environment, as in any shell */
    char *extra[16];
    int nextra = 0;
    while (argc > 1 && nextra < 16) {
        char *eq = strchr(argv[0], '=');
        if (!eq || eq == argv[0]) break;
        int ok = 1;
        for (char *q = argv[0]; q < eq; q++) if (!((*q >= 'A' && *q <= 'Z') || (*q >= 'a' && *q <= 'z') || (*q >= '0' && *q <= '9') || *q == '_')) ok = 0;
        if (!ok) break;
        extra[nextra++] = argv[0];
        memmove(argv, argv + 1, sizeof argv[0] * (size_t)(--argc));
    }
    int background = argc > 1 && !strcmp(argv[argc - 1], "&");   /* `prog &`: don't wait for it */
    if (background) argc--;
    if (!nextra && !background)
        for (const struct cmd *c = cmds; c->name; c++)
            if (!strcmp(c->name, argv[0])) { c->fn(argc, argv); return; }
    /* a program from the root filesystem: real Linux binaries (gcc, make, busybox …) */
    char path[VFS_PATH];
    struct node *n = 0;
    if (strchr(argv[0], '/')) { abspath(argv[0], path, sizeof path); fs_lookup(0, path, 1, &n); }
    else {
        static const char *const dirs[] = { "/usr/local/bin", "/usr/bin", "/usr/sbin", 0 };
        for (int i = 0; dirs[i] && !n; i++) {
            ksnprintf(path, sizeof path, "%s/%s", dirs[i], argv[0]);
            if (fs_lookup(0, path, 1, &n) < 0 || n->type != N_REG) n = 0;
        }
    }
    if (!n || n->type != N_REG) { kprintf("auxsh: %s: command not found (try help)\n", argv[0]); return; }
    char pwd[VFS_PATH + 8];
    ksnprintf(pwd, sizeof pwd, "PWD=%s", cwd);
    char *envp[] = { "PATH=/usr/local/bin:/usr/bin:/usr/sbin", "HOME=/home/root", "USER=root", "LOGNAME=root", "SHELL=/bin/sh",
                     "TERM=linux", "LANG=C.UTF-8", "TMPDIR=/tmp", pwd,
                     "XDG_RUNTIME_DIR=/run/user/0", "WAYLAND_DISPLAY=wayland-0", "GDK_BACKEND=wayland", "GSK_RENDERER=cairo",
                     "GDK_DEBUG=gl-disable,vulkan-disable", "GTK_A11Y=none", "NO_AT_BRIDGE=1", "DBUS_SESSION_BUS_ADDRESS=disabled:",
                     "XDG_DATA_DIRS=/usr/share", 0 };
    char *env[64];
    int ne = 0;
    for (int i = 0; envp[i]; i++) {                              /* defaults, minus the ones given */
        size_t kl = (size_t)(strchr(envp[i], '=') - envp[i]);
        int over = 0;
        for (int j = 0; j < nextra; j++) if (!strncmp(extra[j], envp[i], kl + 1)) over = 1;
        if (!over) env[ne++] = envp[i];
    }
    for (int j = 0; j < nextra; j++) env[ne++] = extra[j];
    env[ne] = 0;
    argv[argc] = 0;
    if (background) {
        int pid = uproc_spawn_detached(path, argv, env, cwd);
        if (pid < 0) kprintf("auxsh: %s: can't start it (error %d)\n", argv[0], -pid);
        else kprintf("[%d] %s\n", pid, argv[0]);
        return;
    }
    int pid = uproc_spawn(path, argv, env, cwd);
    if (pid < 0) { kprintf("auxsh: %s: can't start it (error %d)\n", argv[0], -pid); return; }
    int st = 0;
    uproc_wait(pid, &st);
    if (st & 0x7F) kprintf("auxsh: %s: killed by signal %d\n", argv[0], st & 0x7F);
    else if (st == (127 << 8)) kprintf("auxsh: %s: couldn't run (not a Linux x86_64 program?)\n", argv[0]);
}

void shell_main(uintptr_t arg)
{
    (void)arg;
    static char line[LINE_MAX];
    usb_wait_settled(3000);           /* USB keyboards ready (and announced) before the prompt */
    if (auxo.motd) { char *a[] = { "auxo-motd", 0 }; cmd_auxo_motd(1, a); }
    else kprintf("\n");
    for (;;) {
        if (shell_readline(0, line, sizeof line) > 0) run(line);
    }
}
