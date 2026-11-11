/* auxo-tweak: change your AuxoBSD system, no reboot needed.
 * The command line follows auxo-tweak on Auxo Linux (packages/auxo-tools/files/bin/auxo-tweak):
 * the same sub-command names, the same accents, `info` in the same shape. Sub-commands
 * that need a userland, drivers or a package manager say so instead of failing silently. */
#include "kern.h"
#include "auxo.h"

static const char USAGE[] =
    "auxo-tweak — change your AuxoBSD system, no reboot needed.\n"
    "\n"
    "Run with no arguments for the interactive menu, or use a sub-command:\n"
    "\n"
    "  auxo-tweak accent [NAME]      console, prompt, auxo-fetch and MOTD colour\n"
    "  auxo-tweak hostname [NAME]    the name in the prompt and auxo-fetch\n"
    "  auxo-tweak motd on|off        show auxo-motd when the shell starts\n"
    "  auxo-tweak color on|off       ANSI colour on the serial console\n"
    "  auxo-tweak info               show current Auxo settings\n"
    "  auxo-tweak persist            the GRUB line that keeps these settings after a reboot\n"
    "  auxo-tweak reset              back to the defaults\n";

/* auxo-tweak sub-commands that exist on Auxo Linux but need things AuxoBSD doesn't have yet */
static const char *const LINUX_ONLY[] = {
    "desktop", "rice", "kernel", "shell", "drivers", "nvidia", "scheduler", "power", "firewall",
    "dns", "snapshots", "gaming", "flatpak", "cleanup", "mirrors", "zram", "repo", "service",
    "session-apply", "summit", 0,
};

static int on_off(const char *s)
{
    if (!strcmp(s, "on") || !strcmp(s, "1") || !strcmp(s, "yes")) return 1;
    if (!strcmp(s, "off") || !strcmp(s, "0") || !strcmp(s, "no")) return 0;
    return -1;
}

static void log_ok(const char *what, const char *value)
{
    c_rgb(auxo_c1()); kprintf("▲"); c_reset();
    kprintf(" %s: \033[1m%s\033[0m\n", what, value);
}

static void list_accents(void)
{
    for (int i = 0; i < AUXO_ACCENT_COUNT; i++) {
        uint32_t c = auxo_accents[i].c1;
        c_rgb(c);
        kprintf("  %s %-8s #%06x", i == auxo.accent ? "●" : "○", auxo_accents[i].name, c);
        c_reset();
        kprintf("\n");
    }
}

static int set_accent(const char *name)
{
    int i = auxo_accent_find(name);
    if (i < 0) {
        kprintf("auxo-tweak: unknown accent '%s' (choose from:", name);
        for (int k = 0; k < AUXO_ACCENT_COUNT; k++) kprintf(" %s", auxo_accents[k].name);
        kprintf(")\n");
        return 1;
    }
    auxo_set_accent(i);
    log_ok("accent", name);
    c_dim(); kprintf("  console, prompt, auxo-fetch and the MOTD now use %s\n", name); c_reset();
    return 0;
}

static int set_hostname(const char *name)
{
    size_t n = strlen(name);
    int ok = n > 0 && n < sizeof auxo.hostname && name[0] != '-';
    for (size_t i = 0; ok && i < n; i++) {
        char c = name[i];
        ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-';
    }
    if (!ok) {
        kprintf("auxo-tweak: '%s' isn't a valid hostname (letters, digits and -, up to %lu characters)\n",
                name, (unsigned long)(sizeof auxo.hostname - 1));
        return 1;
    }
    strlcpy(auxo.hostname, name, sizeof auxo.hostname);
    log_ok("hostname", name);
    return 0;
}

static int set_flag(const char *what, int *flag, const char *arg)
{
    int v = on_off(arg);
    if (v < 0) { kprintf("auxo-tweak %s: expected on or off, got '%s'\n", what, arg); return 1; }
    *flag = v;
    if (flag == &auxo.color) console_serial_color(v);
    log_ok(what, v ? "on" : "off");
    return 0;
}

static void info(void)
{
    kprintf("\033[1mAuxo settings\033[0m\n");
    kprintf("  %-10s ", "accent");   c_rgb(auxo_c1()); kprintf("%s", auxo_accent_name()); c_reset(); kprintf("\n");
    kprintf("  %-10s %s\n", "hostname", auxo.hostname);
    kprintf("  %-10s %s\n", "motd", auxo.motd ? "on" : "off");
    kprintf("  %-10s %s\n", "color", auxo.color ? "on" : "off");
    kprintf("  %-10s %s %s\n", "kernel", AUXO_NAME, AUXO_VERSION);
    kprintf("  %-10s %s\n", "cmdline", boot_cmdline()[0] ? boot_cmdline() : "-");
}

static void persist(void)
{
    kprintf("AuxoBSD has no disk to save settings to yet; the kernel command line keeps them.\n"
            "Put this in iso/boot/grub/grub.cfg (or press e at the GRUB menu and edit the line):\n\n  ");
    c_rgb(auxo_c1());
    kprintf("multiboot2 /boot/auxobsd.elf accent=%s hostname=%s%s%s", auxo_accent_name(), auxo.hostname,
            auxo.motd ? "" : " motd=0", auxo.color ? "" : " nocolor");
    c_reset();
    kprintf("\n\n");
}

static void reset(void)
{
    auxo_conf_init("");
    log_ok("settings", "defaults");
}

/* ── interactive menu (auxo-tweak with no arguments) ───────────────── */
static int menu(void)
{
    char in[40];
    for (;;) {
        kprintf("\n\033[1mauxo-tweak\033[0m\n");
        kprintf("  1) accent    "); c_rgb(auxo_c1()); kprintf("%s", auxo_accent_name()); c_reset(); kprintf("\n");
        kprintf("  2) hostname  %s\n", auxo.hostname);
        kprintf("  3) motd      %s\n", auxo.motd ? "on" : "off");
        kprintf("  4) color     %s\n", auxo.color ? "on" : "off");
        kprintf("  5) info\n  6) persist\n  q) quit\n");
        if (shell_readline("choose: ", in, sizeof in) < 0 || !in[0] || in[0] == 'q') return 0;
        switch (in[0]) {
        case '1':
            list_accents();
            if (shell_readline("accent: ", in, sizeof in) > 0) set_accent(in);
            break;
        case '2':
            if (shell_readline("hostname: ", in, sizeof in) > 0) set_hostname(in);
            break;
        case '3': auxo.motd = !auxo.motd; log_ok("motd", auxo.motd ? "on" : "off"); break;
        case '4':
            auxo.color = !auxo.color; console_serial_color(auxo.color);
            log_ok("color", auxo.color ? "on" : "off");
            break;
        case '5': info(); break;
        case '6': persist(); break;
        default:  kprintf("pick 1-6 or q\n");
        }
    }
}

int cmd_auxo_tweak(int argc, char **argv)
{
    if (argc < 2) return menu();
    const char *sub = argv[1], *arg = argc > 2 ? argv[2] : 0;

    if (!strcmp(sub, "help") || !strcmp(sub, "-h") || !strcmp(sub, "--help")) { kprintf("%s", USAGE); return 0; }
    if (!strcmp(sub, "accent"))   { if (!arg) { list_accents(); return 0; } return set_accent(arg); }
    if (!strcmp(sub, "hostname")) { if (!arg) { kprintf("%s\n", auxo.hostname); return 0; } return set_hostname(arg); }
    if (!strcmp(sub, "motd"))     { if (!arg) { kprintf("%s\n", auxo.motd ? "on" : "off"); return 0; } return set_flag("motd", &auxo.motd, arg); }
    if (!strcmp(sub, "color") || !strcmp(sub, "colour")) {
        if (!arg) { kprintf("%s\n", auxo.color ? "on" : "off"); return 0; }
        return set_flag("color", &auxo.color, arg);
    }
    if (!strcmp(sub, "info"))     { info(); return 0; }
    if (!strcmp(sub, "persist"))  { persist(); return 0; }
    if (!strcmp(sub, "reset"))    { reset(); return 0; }

    for (const char *const *l = LINUX_ONLY; *l; l++)
        if (!strcmp(sub, *l)) {
            kprintf("auxo-tweak %s is part of Auxo Linux. AuxoBSD doesn't have it yet "
                    "(no userland, package manager or GPU drivers so far).\n", sub);
            return 1;
        }
    kprintf("auxo-tweak: unknown command '%s'\n\n%s", sub, USAGE);
    return 1;
}
