/* auxsh: the AuxoBSD kernel shell. Runs as a kernel thread; commands are built in
 * until there is a userland. Line editing: Backspace, Ctrl-C, Ctrl-U, Ctrl-L,
 * Up/Down history, Tab completes command names. */
#include "kern.h"
#include "arch.h"
#include "rs.h"
#include "auxo.h"

#define LINE_MAX 160
#define HIST     16
#define MAXARGS  16

struct cmd {
    const char *name;
    int (*fn)(int argc, char **argv);
    const char *help;
};

static const struct cmd cmds[];

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
    { "demo",        cmd_demo,        "run the scheduler / syscall demo threads" },
    { "cmdline",     cmd_cmdline,     "the kernel command line from the boot loader" },
    { "history",     cmd_history,     "recent commands" },
    { "reboot",      cmd_reboot,      "restart the machine" },
    { "poweroff",    cmd_poweroff,    "turn the machine off" },
    { "halt",        cmd_poweroff,    0 },
    { 0, 0, 0 },
};

static void run(char *line)
{
    char *argv[MAXARGS];
    int argc = split(line, argv);
    if (!argc) return;
    for (const struct cmd *c = cmds; c->name; c++)
        if (!strcmp(c->name, argv[0])) { c->fn(argc, argv); return; }
    kprintf("auxsh: %s: command not found (try help)\n", argv[0]);
}

void shell_main(uintptr_t arg)
{
    (void)arg;
    static char line[LINE_MAX];
    if (auxo.motd) { char *a[] = { "auxo-motd", 0 }; cmd_auxo_motd(1, a); }
    else kprintf("\n");
    for (;;) {
        if (shell_readline(0, line, sizeof line) > 0) run(line);
    }
}
