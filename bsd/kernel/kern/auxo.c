/* Auxo settings on AuxoBSD and the helpers the Auxo tools share.
 *
 * There is no filesystem yet, so settings live in memory and start from the kernel
 * command line, the same keys `auxo-tweak persist` prints for the GRUB entry:
 *   accent=rose hostname=summit motd=0 nocolor                                      */
#include "kern.h"
#include "arch.h"
#include "auxo.h"

struct auxo_settings auxo;

int auxo_accent_find(const char *name)
{
    for (int i = 0; i < AUXO_ACCENT_COUNT; i++)
        if (!strcmp(auxo_accents[i].name, name)) return i;
    return -1;
}

const char *auxo_accent_name(void) { return auxo_accents[auxo.accent].name; }
uint32_t auxo_c1(void) { return auxo_accents[auxo.accent].c1; }
uint32_t auxo_c2(void) { return auxo_accents[auxo.accent].c2; }

void auxo_set_accent(int idx)
{
    if (idx < 0 || idx >= AUXO_ACCENT_COUNT) return;
    auxo.accent = idx;
    console_set_accent(auxo_c1(), auxo_c2());
}

static void apply_kv(const char *kv)
{
    char key[16], val[32];
    const char *eq = strchr(kv, '=');
    size_t kl = eq ? (size_t)(eq - kv) : strlen(kv);
    if (kl >= sizeof key) return;
    memcpy(key, kv, kl);
    key[kl] = 0;
    strlcpy(val, eq ? eq + 1 : "", sizeof val);

    if (!strcmp(key, "accent")) {
        int i = auxo_accent_find(val);
        if (i >= 0) auxo.accent = i;
    } else if (!strcmp(key, "hostname") && val[0]) {
        strlcpy(auxo.hostname, val, sizeof auxo.hostname);
    } else if (!strcmp(key, "motd")) {
        auxo.motd = strcmp(val, "0") && strcmp(val, "off");
    } else if (!strcmp(key, "nocolor") || !strcmp(key, "NO_COLOR")) {
        auxo.color = 0;
    } else if (!strcmp(key, "color")) {
        auxo.color = strcmp(val, "0") && strcmp(val, "off");
    }
}

void auxo_conf_init(const char *cmdline)
{
    auxo.accent = AUXO_ACCENT_DEFAULT;
    strlcpy(auxo.hostname, "auxobsd", sizeof auxo.hostname);
    auxo.motd = 1;
    auxo.color = 1;

    char tok[64];
    while (*cmdline) {
        while (*cmdline == ' ') cmdline++;
        size_t n = 0;
        while (*cmdline && *cmdline != ' ') { if (n + 1 < sizeof tok) tok[n++] = *cmdline; cmdline++; }
        tok[n] = 0;
        if (n) apply_kv(tok);
    }
    console_serial_color(auxo.color);
    console_set_accent(auxo_c1(), auxo_c2());
}

/* ── colour helpers ─────────────────────────────────────────────────── */
uint32_t rgb_lerp(uint32_t a, uint32_t b, int num, int den)
{
    if (den <= 0) return a;
    uint32_t out = 0;
    for (int sh = 16; sh >= 0; sh -= 8) {
        int x = (int)((a >> sh) & 0xFF), y = (int)((b >> sh) & 0xFF);
        out |= (uint32_t)(x + (y - x) * num / den) << sh;
    }
    return out;
}

void c_rgb(uint32_t rgb)
{
    kprintf("\033[38;2;%u;%u;%um", (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

void c_rgb_bold(uint32_t rgb)
{
    kprintf("\033[1;38;2;%u;%u;%um", (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

void c_dim(void)   { kprintf("\033[38;5;245m"); }
void c_bold(void)  { kprintf("\033[1m"); }
void c_reset(void) { kprintf("\033[0m"); }

/* ── system facts ───────────────────────────────────────────────────── */
void fmt_uptime(char *buf, size_t n)
{
    uint64_t s = kernel_ticks() / AUXO_HZ;
    unsigned long d = (unsigned long)(s / 86400), h = (unsigned long)(s % 86400 / 3600),
                  m = (unsigned long)(s % 3600 / 60);
    size_t o = 0;
    if (d) o += (size_t)ksnprintf(buf + o, n - o, "%lud ", d);
    if (h && o < n) o += (size_t)ksnprintf(buf + o, n - o, "%luh ", h);
    if (o < n) ksnprintf(buf + o, n - o, "%lum", m);
}

const char *cpu_brand(void)
{
    static char brand[49];
    if (brand[0]) return brand;
    uint32_t r[4];
    cpuid(0x80000000, r);
    if (r[0] >= 0x80000004) {
        char raw[49];
        for (uint32_t i = 0; i < 3; i++) {
            cpuid(0x80000002 + i, r);
            memcpy(raw + i * 16, r, 16);
        }
        raw[48] = 0;
        /* tidy it the way auxo-fetch on Linux does: drop (R), (TM), "CPU", "@ 3.40GHz" */
        size_t o = 0;
        for (const char *p = raw; *p && o < sizeof brand - 1; ) {
            if (!strncmp(p, "(R)", 3)) { p += 3; continue; }
            if (!strncmp(p, "(TM)", 4)) { p += 4; continue; }
            if (!strncmp(p, " CPU", 4) && (p[4] == ' ' || p[4] == 0)) { p += 4; continue; }
            if (*p == '@') break;
            if (*p == ' ' && (o == 0 || brand[o - 1] == ' ')) { p++; continue; }
            brand[o++] = *p++;
        }
        while (o && brand[o - 1] == ' ') o--;
        brand[o] = 0;
    }
    if (!brand[0]) {
        cpuid(0, r);
        memcpy(brand, &r[1], 4); memcpy(brand + 4, &r[3], 4); memcpy(brand + 8, &r[2], 4);
        brand[12] = 0;
    }
    return brand;
}

int rand_range(int n)
{
    static uint64_t x;
    if (!x) x = rdtsc() | 1;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    return n > 0 ? (int)(x % (uint64_t)n) : 0;
}
