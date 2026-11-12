/* auxo-fetch: a fast system summary drawn in your Auxo accent colour.
 * Same logo, layout and flags as packages/auxo-tools/files/bin/auxo-fetch on Auxo Linux. */
#include "kern.h"
#include "rs.h"
#include "auxo.h"
#include "cred.h"

static const char *const LOGO[] = {
    "         ●",
    "        /\\",
    "       /  \\",
    "      /    \\",
    "     /  /\\  \\",
    "    /  /  \\  \\",
    "   /  / /\\ \\  \\",
    "  /__/ /  \\ \\__\\",
};
static const char *const SMALL[] = { "   ●  ", "  /\\  ", " /  \\ ", "/_/\\_\\" };

#define NLINES 16
#define LINE   640
static char lines[NLINES][LINE];
static int  nlines;

/* display width of a UTF-8 string without escapes */
static int width(const char *s)
{
    int w = 0;
    for (; *s; s++) if (((uint8_t)*s & 0xC0) != 0x80) w++;
    return w;
}

static size_t put_rgb(char *b, size_t n, uint32_t c, int bold)
{
    return (size_t)ksnprintf(b, n, "\033[%s38;2;%u;%u;%um", bold ? "1;" : "", (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
}

static char *add(void)
{
    char *l = lines[nlines < NLINES ? nlines++ : NLINES - 1];
    l[0] = 0;
    return l;
}

static void field(const char *label, const char *value)
{
    char *l = add();
    size_t o = put_rgb(l, LINE, auxo_c1(), 0);
    ksnprintf(l + o, LINE - o, "%-8s\033[0m %s", label, value);
}

/* gradient bar like auxo-fetch on Linux: ━ in accent shades, the rest dim */
static void bar(const char *label, unsigned long used, unsigned long total, const char *unit)
{
    const int w = 14;
    int fill = total ? (int)((used * w + total / 2) / total) : 0;
    char *l = add();
    size_t o = put_rgb(l, LINE, auxo_c1(), 0);
    o += (size_t)ksnprintf(l + o, LINE - o, "%-8s\033[0m ", label);
    for (int i = 0; i < fill; i++) {
        o += put_rgb(l + o, LINE - o, rgb_lerp(auxo_c1(), auxo_c2(), i, w - 1), 0);
        o += (size_t)ksnprintf(l + o, LINE - o, "━");
    }
    o += (size_t)ksnprintf(l + o, LINE - o, "\033[38;5;238m");
    for (int i = fill; i < w; i++) o += (size_t)ksnprintf(l + o, LINE - o, "━");
    ksnprintf(l + o, LINE - o, "\033[0m %lu/%lu %s", used, total, unit);
}

static void json_str(const char *k, const char *v, int comma)
{
    kprintf("  \"%s\": \"", k);
    for (; *v; v++) {
        if (*v == '"' || *v == '\\') kprintf("\\%c", *v);
        else if ((uint8_t)*v >= 0x20) kprintf("%c", *v);
    }
    kprintf("\"%s\n", comma ? "," : "");
}

static void usage(void)
{
    kprintf("usage: auxo-fetch [--json] [--small] [--no-logo] [--accent NAME]\n");
}

int cmd_auxo_fetch(int argc, char **argv)
{
    int json = 0, small = 0, nologo = 0, acc = auxo.accent;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--json")) json = 1;
        else if (!strcmp(argv[i], "--small")) small = 1;
        else if (!strcmp(argv[i], "--no-logo")) nologo = 1;
        else if (!strcmp(argv[i], "--accent") && i + 1 < argc) {
            acc = auxo_accent_find(argv[++i]);
            if (acc < 0) { kprintf("auxo-fetch: unknown accent '%s'\n", argv[i]); return 1; }
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(); return 0; }
        else { usage(); return 1; }
    }

    char user[48], os[48], kern[64], up[32], procs[32], boot[80];
    ksnprintf(user, sizeof user, "%s@%s", session_user(), auxo.hostname);
    ksnprintf(os, sizeof os, "%s %s x86_64", AUXO_NAME, AUXO_VERSION);
    ksnprintf(kern, sizeof kern, "auxobsd %s (C + Rust + asm)", AUXO_VERSION);
    fmt_uptime(up, sizeof up);
    unsigned long np = (unsigned long)rs_proc_count();
    ksnprintf(procs, sizeof procs, "%lu kernel thread%s", np, np == 1 ? "" : "s");
    ksnprintf(boot, sizeof boot, "%s (multiboot2)", boot_loader()[0] ? boot_loader() : "unknown");
    unsigned long mt = (unsigned long)(pmm_total_pages() * PAGE_SIZE >> 20);
    unsigned long mu = mt - (unsigned long)(pmm_free_pages() * PAGE_SIZE >> 20);
    size_t ht, hu;
    rs_heap_stats(&ht, &hu);

    if (json) {
        kprintf("{\n");
        json_str("user", user, 1); json_str("os", os, 1); json_str("kernel", kern, 1);
        json_str("uptime", up, 1);
        kprintf("  \"threads\": %lu,\n", np);
        json_str("shell", "auxsh", 1); json_str("boot", boot, 1); json_str("cpu", cpu_brand(), 1);
        kprintf("  \"memory_mib\": [%lu, %lu],\n", mu, mt);
        kprintf("  \"heap_kib\": [%lu, %lu],\n", (unsigned long)(hu >> 10), (unsigned long)(ht >> 10));
        json_str("accent", auxo_accents[acc].name, 0);
        kprintf("}\n");
        return 0;
    }

    int saved = auxo.accent;
    auxo.accent = acc;                         /* --accent only changes this drawing */
    uint32_t c1 = auxo_c1(), c2 = auxo_c2();
    nlines = 0;

    char *l = add();
    size_t o = put_rgb(l, LINE, c1, 1);
    ksnprintf(l + o, LINE - o, "%s\033[0m", user);
    if (!small) {
        l = add();
        o = (size_t)ksnprintf(l, LINE, "\033[38;5;245m");
        for (int i = 0; i < width(user); i++) o += (size_t)ksnprintf(l + o, LINE - o, "─");
        ksnprintf(l + o, LINE - o, "\033[0m");
    }
    field("os", os);
    field("kernel", kern);
    field("uptime", up);
    field("procs", procs);
    field("shell", "auxsh");
    field("boot", boot);
    if (!small) {
        field("cpu", cpu_brand());
        bar("memory", mu, mt, "MiB");
        bar("heap", (unsigned long)(hu >> 10), (unsigned long)(ht >> 10), "KiB");
        l = add();
        o = put_rgb(l, LINE, c1, 0);
        o += (size_t)ksnprintf(l + o, LINE - o, "%-8s\033[0m ", "accent");
        o += put_rgb(l + o, LINE - o, c1, 0);
        ksnprintf(l + o, LINE - o, "●\033[0m %s", auxo_accents[acc].name);
        add();
        l = add();
        o = 0;
        for (int i = 0; i < 8; i++) {
            o += put_rgb(l + o, LINE - o, rgb_lerp(c1, c2, i, 7), 0);
            o += (size_t)ksnprintf(l + o, LINE - o, "███");
        }
        ksnprintf(l + o, LINE - o, "\033[0m");
    }

    const char *const *logo = nologo ? 0 : small ? SMALL : LOGO;
    int nl = nologo ? 0 : small ? 4 : 8, w = 0;
    for (int i = 0; i < nl; i++) if (width(logo[i]) > w) w = width(logo[i]);

    kprintf("\n");
    int rows = nl > nlines ? nl : nlines;
    for (int i = 0; i < rows; i++) {
        kprintf("  ");
        if (i < nl) {
            c_rgb_bold(rgb_lerp(c1, c2, i, nl > 1 ? nl - 1 : 1));
            kprintf("%s", logo[i]);
            c_reset();
        }
        for (int pad = (i < nl ? width(logo[i]) : 0); pad < w; pad++) kprintf(" ");
        kprintf("%s%s\n", nl ? "   " : "", i < nlines ? lines[i] : "");
    }
    kprintf("\n");
    auxo.accent = saved;
    return 0;
}
