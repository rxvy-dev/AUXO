/* auxo-motd: the Auxo greeting, shown when auxsh starts (like a new terminal on Auxo Linux).
 * Same logo, colours and Terry Fox Foundation note as packages/auxo-tools/files/bin/auxo-motd. */
#include "kern.h"
#include "auxo.h"

static const char *const LOGO[] = { "   ●  ", "  /\\  ", " /  \\ ", "/_/\\_\\" };

static const char *const TIPS[] = {
    "auxo-tweak accent rose — recolour the console, prompt, auxo-fetch and this greeting at once",
    "auxo-fetch — your system, drawn in your accent colour",
    "auxo-fetch --small — the short version (--json for scripts)",
    "auxo-tweak hostname summit — rename this machine",
    "auxo-tweak persist — the GRUB line that keeps your settings after a reboot",
    "auxo-tweak info — every Auxo setting at a glance",
    "ps — every kernel thread and how long it has run",
    "demo — watch the Rust scheduler juggle three threads",
    "free — physical memory and the Rust kernel heap",
    "Tab completes commands, Up/Down walks your history",
    "auxo-motd --off — hide this greeting (auxo-motd --on brings it back)",
};
#define NTIPS (int)(sizeof TIPS / sizeof TIPS[0])

static const char GIVE[] = "Auxo is free. Remember you can donate to cancer research: terryfox.org";

int cmd_auxo_motd(int argc, char **argv)
{
    int tip_only = 0, force = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--give")) {
            kprintf("Auxo is free, and Auxo supports cancer research. If Auxo is useful to you, please give\n"
                    "to the Terry Fox Foundation instead: https://terryfox.org/?form=GENERAL\n");
            return 0;
        }
        if (!strcmp(argv[i], "--off")) {
            auxo.motd = 0;
            kprintf("auxo-motd: off (auxo-motd --on brings it back; auxo-tweak persist keeps it after a reboot)\n");
            return 0;
        }
        if (!strcmp(argv[i], "--on")) { auxo.motd = 1; kprintf("auxo-motd: on\n"); return 0; }
        if (!strcmp(argv[i], "--tip")) tip_only = 1;
        else if (!strcmp(argv[i], "--force")) force = 1;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            kprintf("auxo-motd          show it\n"
                    "auxo-motd --tip    show just a random tip\n"
                    "auxo-motd --off    stop showing it when the shell starts (--on brings it back)\n"
                    "auxo-motd --give   how to support cancer research (Auxo supports the Terry Fox Foundation)\n");
            return 0;
        }
    }
    (void)force;                               /* typing auxo-motd always shows it */

    int give = rand_range(100) < 25;           /* about one shell in four, like Auxo Linux */
    const char *text = give ? GIVE : TIPS[rand_range(NTIPS)];
    if (tip_only) { kprintf("%s\n", text); return 0; }

    char up[32];
    fmt_uptime(up, sizeof up);
    unsigned long t = (unsigned long)pmm_total_pages(), f = (unsigned long)pmm_free_pages();
    unsigned long pct = t ? (100 * (t - f) + t / 2) / t : 0;
    uint32_t c1 = auxo_c1(), c2 = auxo_c2();

    kprintf("\n");
    for (int i = 0; i < 4; i++) {
        c_rgb_bold(i < 2 ? c1 : c2);
        kprintf("%s", LOGO[i]);
        c_reset();
        kprintf("  ");
        switch (i) {
        case 0: kprintf("\033[1mWelcome to %s, root.\033[0m", AUXO_NAME); break;
        case 1: c_dim(); kprintf("%s %s · up %s · mem %lu%% used", AUXO_NAME, AUXO_VERSION, up, pct); c_reset(); break;
        case 2:
            c_dim(); kprintf("type "); c_reset(); c_rgb(c1); kprintf("help"); c_reset();
            c_dim(); kprintf(" for commands · "); c_reset(); c_rgb(c1); kprintf("auxo-fetch"); c_reset();
            c_dim(); kprintf(" for a summary"); c_reset();
            break;
        case 3:
            if (give) { kprintf("\033[38;2;255;90;100m♥\033[0m %s", text); }
            else { c_dim(); kprintf("tip:"); c_reset(); kprintf(" %s", text); }
            break;
        }
        kprintf("\n");
    }
    kprintf("\n");
    return 0;
}
