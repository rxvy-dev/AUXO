/* The AuxoBSD installer, a Chrono app: pick a disk, make your account, name the computer, install.
 * The writing itself happens on the "installer" kernel thread (disk/install.c). */
#include "kern.h"
#include "gfx.h"
#include "fonts.h"
#include "apps.h"
#include "auxo.h"
#include "disk.h"
#include "install.h"
#include "cred.h"

enum { S_WELCOME, S_DISK, S_ACCOUNT, S_OPTIONS, S_RUN, S_DONE };
static int step, sel = -1, accent, agree, seen_state;
static char host[32] = "auxobsd";
static int host_focus;
/* the account: full name, user name, password twice. focus: 1..4 */
static char fullname[64], uname[33], pw1[128], pw2[128];
static int acc_focus = 1, uname_typed;

static const char *account_problem(void)
{
    if (!fullname[0] && !uname[0]) return "Type your name.";
    if (!uname[0]) return "Choose a user name.";
    if (uname[0] < 'a' || uname[0] > 'z') return "A user name starts with a lowercase letter.";
    struct account a;
    if (strcmp(uname, session_user()) && account_by_name(uname, &a) == 0) return "That user name is taken by the system. Choose another.";
    if (!pw1[0]) return "Choose a password.";
    if (strcmp(pw1, pw2)) return "The two passwords don't match.";
    return 0;
}

/* the user name follows the first word of the full name until it's typed itself */
static void suggest_uname(void)
{
    if (uname_typed) return;
    size_t o = 0;
    for (const char *p = fullname; *p && *p != ' ' && o + 1 < sizeof uname; p++) {
        char ch = *p >= 'A' && *p <= 'Z' ? (char)(*p + 32) : *p;
        if ((ch >= 'a' && ch <= 'z') || (o && ch >= '0' && ch <= '9')) uname[o++] = ch;
    }
    uname[o] = 0;
}

static int W_, H_;

struct btn { int x, y, w, h; };
static struct btn next_b, back_b, agree_b, host_b, restart_b;
static struct btn disk_b[16], accent_b[16];

static int in(const struct btn *b, int x, int y) { return x >= b->x && y >= b->y && x < b->x + b->w && y < b->y + b->h; }

static void button(struct canvas *c, const struct app_ctx *a, struct btn *b, int x, int y, int w, const char *label, uint32_t bg, uint32_t fg)
{
    *b = (struct btn){ x - a->x, y - a->y, w, 34 };
    gfx_round(c, x, y, w, 34, 8, bg, 0);
    gfx_text(c, &font_bold, x + (w - gfx_text_width(&font_bold, label)) / 2, y + 9, label, fg);
}

static struct btn acc_b[4];

static void field(struct canvas *c, const struct app_ctx *a, struct btn *b, int x, int y, int w, const char *label, const char *val, int secret, int focus)
{
    const struct app_colors *k = a->col;
    gfx_text(c, &font_bold, x, y, label, k->fg);
    *b = (struct btn){ x - a->x, y + 22 - a->y, w, 32 };
    gfx_round(c, x, y + 22, w, 32, 6, k->field, 0);
    gfx_round_frame(c, x, y + 22, w, 32, 6, focus ? k->accent : with_alpha(k->fg, 60));
    char shown[130];
    if (secret) { size_t n = strlen(val); if (n > 40) n = 40; for (size_t i = 0; i < n; i++) shown[i] = '*'; shown[n] = 0; }
    else strlcpy(shown, val, sizeof shown);
    int tw = gfx_text(c, &font_ui, x + 10, y + 29, shown, k->fg);
    if (focus && a->focused && !(a->blink & 1)) gfx_fill(c, x + 11 + tw, y + 29, 1, 16, k->fg);
}

/* word-wrapped text; returns the y below the last line */
static int wrap(struct canvas *c, const struct font *f, int x, int y, int w, const char *t, uint32_t col)
{
    char line[256];
    while (*t) {
        size_t n = 0, fit = 0;
        while (t[n] && t[n] != '\n' && n < sizeof line - 1) {
            line[n] = t[n]; line[n + 1] = 0;
            if (gfx_text_width(f, line) > w) break;
            n++;
            if (!t[n] || t[n] == ' ' || t[n] == '\n') fit = n;
        }
        if (!t[n] || t[n] == '\n') fit = n;      /* the rest fits */
        if (!fit) fit = n ? n : 1;                 /* one long word: hard break */
        memcpy(line, t, fit); line[fit] = 0;
        gfx_text(c, f, x, y, line, col);
        y += f == &font_ui ? 22 : 24;
        t += fit;
        while (*t == ' ') t++;
        if (*t == '\n') t++;
    }
    return y;
}

static void size_str(uint64_t sectors, char *b, size_t n)
{
    uint64_t mb = sectors / 2048;
    if (mb >= 1024 * 1024) ksnprintf(b, n, "%lu.%lu TB", (unsigned long)(mb / 1048576), (unsigned long)(mb % 1048576 * 10 / 1048576));
    else if (mb >= 1024) ksnprintf(b, n, "%lu GB", (unsigned long)(mb / 1024));
    else ksnprintf(b, n, "%lu MB", (unsigned long)mb);
}

void installer_paint(const struct app_ctx *a)
{
    struct canvas *c = a->c;
    const struct app_colors *k = a->col;
    int X = a->x, Y = a->y;
    W_ = a->w; H_ = a->h;
    gfx_fill(c, X, Y, a->w, a->h, k->bg);
    /* left rail: the steps */
    gfx_fill(c, X, Y, 190, a->h, k->field);
    gfx_text(c, &font_head, X + 22, Y + 22, "AuxoBSD", k->fg);
    static const char *const steps[] = { "Welcome", "Disk", "You", "Options", "Install", "Done" };
    for (int i = 0; i < 6; i++) {
        uint32_t col = i == step ? k->accent : i < step ? k->fg : k->dim;
        gfx_circle(c, X + 30, Y + 86 + i * 34, 6, col);
        gfx_text(c, i == step ? &font_bold : &font_ui, X + 46, Y + 78 + i * 34, steps[i], col);
    }
    int cx = X + 220, cy = Y + 28, cw = a->w - 250;
    next_b = back_b = agree_b = host_b = restart_b = (struct btn){ 0, 0, 0, 0 };
    char line[160];

    if (step == S_WELCOME) {
        gfx_text(c, &font_head, cx, cy, "Install AuxoBSD", k->fg);
        int ty = wrap(c, &font_ui, cx, cy + 50, cw, "This puts AuxoBSD on a disk of this computer so it starts without the install "
                      "media, and keeps your files and the programs you install between restarts.", k->dim);
        ty = wrap(c, &font_bold, cx, ty + 14, cw, "The disk you choose is erased completely.", k->danger);
        wrap(c, &font_ui, cx, ty + 14, cw, "It boots on BIOS and UEFI computers. IDE, SATA and NVMe disks are supported; "
             "USB disks aren't yet.", k->dim);
        button(c, a, &next_b, X + a->w - 160, Y + a->h - 58, 130, "Next", k->accent, k->accent_fg);
    } else if (step == S_DISK) {
        gfx_text(c, &font_head, cx, cy, "Choose a disk", k->fg);
        if (!disk_count()) gfx_text_fit(c, &font_ui, cx, cy + 50, cw, "No disks found. (IDE, SATA/AHCI, NVMe and USB are supported.)", k->dim);
        for (int i = 0; i < disk_count() && i < 16; i++) {
            struct disk *d = disk_get(i);
            int y = cy + 50 + i * 66;
            disk_b[i] = (struct btn){ cx - X, y - Y, cw, 58 };
            gfx_round(c, cx, y, cw, 58, 10, i == sel ? k->accent : k->field, 0);
            uint32_t fg = i == sel ? k->accent_fg : k->fg;
            char sz[24];
            size_str(d->sectors, sz, sizeof sz);
            if (d->gone) strlcpy(sz, "unplugged", sizeof sz);
            ksnprintf(line, sizeof line, "%s  ·  %s", d->model[0] ? d->model : "Disk", sz);
            gfx_text_fit(c, &font_bold, cx + 16, y + 10, cw - 32, line, fg);
            ksnprintf(line, sizeof line, "%s  ·  %s%s", d->name, d->bus, d->boot ? "  ·  AuxoBSD is installed here" : "");
            gfx_text_fit(c, &font_ui, cx + 16, y + 32, cw - 32, line, with_alpha(fg, 170));
        }
        button(c, a, &back_b, X + a->w - 300, Y + a->h - 58, 120, "Back", k->field, k->fg);
        button(c, a, &next_b, X + a->w - 160, Y + a->h - 58, 130, "Next", sel >= 0 ? k->accent : k->field, sel >= 0 ? k->accent_fg : k->dim);
    } else if (step == S_ACCOUNT) {
        gfx_text(c, &font_head, cx, cy, "Who will use this computer?", k->fg);
        int fw = cw < 360 ? cw : 360;
        field(c, a, &acc_b[0], cx, cy + 48, fw, "Your name", fullname, 0, acc_focus == 1);
        field(c, a, &acc_b[1], cx, cy + 112, fw, "User name", uname, 0, acc_focus == 2);
        field(c, a, &acc_b[2], cx, cy + 176, fw, "Password", pw1, 1, acc_focus == 3);
        field(c, a, &acc_b[3], cx, cy + 240, fw, "Password again", pw2, 1, acc_focus == 4);
        const char *p = account_problem();
        wrap(c, &font_ui, cx, cy + 310, cw, p ? p : "You'll type this password to unlock the computer and for sudo.", p ? k->dim : k->fg);
        button(c, a, &back_b, X + a->w - 300, Y + a->h - 58, 120, "Back", k->field, k->fg);
        button(c, a, &next_b, X + a->w - 160, Y + a->h - 58, 130, "Next", !p ? k->accent : k->field, !p ? k->accent_fg : k->dim);
    } else if (step == S_OPTIONS) {
        gfx_text(c, &font_head, cx, cy, "Options", k->fg);
        gfx_text(c, &font_bold, cx, cy + 52, "Computer name", k->fg);
        host_b = (struct btn){ cx - X, cy + 74 - Y, 280, 32 };
        gfx_round(c, cx, cy + 74, 280, 32, 6, k->field, 0);
        gfx_round_frame(c, cx, cy + 74, 280, 32, 6, host_focus ? k->accent : with_alpha(k->fg, 60));
        int tw = gfx_text(c, &font_ui, cx + 10, cy + 81, host, k->fg);
        if (host_focus && a->focused && !(a->blink & 1)) gfx_fill(c, cx + 11 + tw, cy + 81, 1, 16, k->fg);
        gfx_text(c, &font_bold, cx, cy + 126, "Auxo accent", k->fg);
        for (int i = 0; i < AUXO_ACCENT_COUNT && i < 16; i++) {
            int bx = cx + i * 40;
            accent_b[i] = (struct btn){ bx - X, cy + 148 - Y, 32, 32 };
            if (i == accent) gfx_ring(c, bx + 16, cy + 164, 15, 2, k->fg);
            gfx_circle(c, bx + 16, cy + 164, 12, auxo_accents[i].c1 | 0xff000000);
        }
        struct disk *d = disk_get(sel);
        char sz[24];
        size_str(d ? d->sectors : 0, sz, sizeof sz);
        ksnprintf(line, sizeof line, "Everything on %s (%s, %s) will be erased.", d ? d->name : "?", d ? d->model : "?", sz);
        int by = wrap(c, &font_bold, cx, cy + 210, cw, line, k->danger) + 6;
        agree_b = (struct btn){ cx - X, by - 2 - Y, cw, 26 };
        gfx_round(c, cx, by, 20, 20, 4, agree ? k->danger : k->field, 0);
        gfx_round_frame(c, cx, by, 20, 20, 4, k->danger);
        if (agree) gfx_text(c, &font_bold, cx + 4, by + 1, "✓", 0xffffffff);
        gfx_text_fit(c, &font_ui, cx + 30, by + 1, cw - 30, "I understand: erase this disk and install AuxoBSD", k->fg);
        button(c, a, &back_b, X + a->w - 300, Y + a->h - 58, 120, "Back", k->field, k->fg);
        button(c, a, &next_b, X + a->w - 160, Y + a->h - 58, 130, "Install", agree ? k->danger : k->field, agree ? 0xffffffff : k->dim);
    } else if (step == S_RUN || step == S_DONE) {
        int failed = install_state == 3;
        gfx_text(c, &font_head, cx, cy, step == S_RUN ? "Installing…" : failed ? "Install failed" : "All done", k->fg);
        gfx_round(c, cx, cy + 60, cw, 14, 7, k->field, 0);
        int pw = cw * (install_pct < 0 ? 0 : install_pct > 100 ? 100 : install_pct) / 100;
        if (pw > 0) gfx_round(c, cx, cy + 60, pw < 14 ? 14 : pw, 14, 7, failed ? k->danger : k->accent, 0);
        ksnprintf(line, sizeof line, "%d%%", install_pct);
        gfx_text(c, &font_ui, cx, cy + 84, line, k->dim);
        wrap(c, &font_ui, cx, cy + 112, cw, install_msg, failed ? k->danger : k->fg);
        if (step == S_DONE && !failed) button(c, a, &restart_b, X + a->w - 160, Y + a->h - 58, 130, "Restart", k->accent, k->accent_fg);
        if (step == S_DONE && failed) button(c, a, &back_b, X + a->w - 160, Y + a->h - 58, 130, "Back", k->field, k->fg);
    }
}

int installer_click(int x, int y, int w, int h)
{
    (void)w; (void)h;
    host_focus = 0;
    switch (step) {
    case S_WELCOME: if (in(&next_b, x, y)) step = S_DISK; break;
    case S_DISK:
        for (int i = 0; i < disk_count() && i < 16; i++) if (in(&disk_b[i], x, y) && !disk_get(i)->gone) sel = i;
        if (in(&back_b, x, y)) step = S_WELCOME;
        if (in(&next_b, x, y) && sel >= 0) step = S_ACCOUNT;
        break;
    case S_ACCOUNT:
        for (int i = 0; i < 4; i++) if (in(&acc_b[i], x, y)) acc_focus = i + 1;
        if (in(&back_b, x, y)) step = S_DISK;
        if (in(&next_b, x, y) && !account_problem()) { step = S_OPTIONS; agree = 0; }
        break;
    case S_OPTIONS:
        if (in(&host_b, x, y)) host_focus = 1;
        for (int i = 0; i < AUXO_ACCENT_COUNT && i < 16; i++) if (in(&accent_b[i], x, y)) accent = i;
        if (in(&agree_b, x, y)) agree = !agree;
        if (in(&back_b, x, y)) step = S_ACCOUNT;
        if (in(&next_b, x, y) && agree && host[0]) {
            struct install_req r;
            memset(&r, 0, sizeof r);
            r.disk = sel; r.accent = accent;
            strlcpy(r.hostname, host, sizeof r.hostname);
            strlcpy(r.user, uname, sizeof r.user);
            strlcpy(r.fullname, fullname, sizeof r.fullname);
            strlcpy(r.password, pw1, sizeof r.password);
            memset(pw1, 0, sizeof pw1); memset(pw2, 0, sizeof pw2);
            if (install_start(&r) == 0) { step = S_RUN; seen_state = 1; }
        }
        break;
    case S_DONE:
        if (in(&restart_b, x, y)) machine_reboot();
        if (in(&back_b, x, y)) { step = S_DISK; install_state = 0; }
        break;
    }
    return 1;
}

int installer_key(int k)
{
    if (step == S_ACCOUNT) {
        char *f = acc_focus == 1 ? fullname : acc_focus == 2 ? uname : acc_focus == 3 ? pw1 : pw2;
        size_t cap = acc_focus == 1 ? sizeof fullname : acc_focus == 2 ? sizeof uname : sizeof pw1, n = strlen(f);
        if (k == '\t' || k == '\r' || k == '\n') {
            if ((k == '\r' || k == '\n') && acc_focus == 4 && !account_problem()) { step = S_OPTIONS; agree = 0; return 1; }
            acc_focus = acc_focus % 4 + 1;
            return 1;
        }
        if (k == '\b' || k == 0x7f) { if (n) f[n - 1] = 0; if (acc_focus == 2) uname_typed = n > 1; if (acc_focus == 1) suggest_uname(); return 1; }
        if (k < 0x20 || k >= 0x7f || n + 1 >= cap) return 0;
        if (acc_focus == 2) {                                   /* user names: a-z 0-9 - _ */
            if (k >= 'A' && k <= 'Z') k += 32;
            if (!((k >= 'a' && k <= 'z') || (k >= '0' && k <= '9') || k == '-' || k == '_')) return 0;
            uname_typed = 1;
        }
        f[n] = (char)k; f[n + 1] = 0;
        if (acc_focus == 1) suggest_uname();
        return 1;
    }
    if (step != S_OPTIONS || !host_focus) return 0;
    size_t n = strlen(host);
    if (k == '\b') { if (n) host[n - 1] = 0; return 1; }
    if (((k >= 'a' && k <= 'z') || (k >= '0' && k <= '9') || k == '-') && n + 1 < sizeof host) { host[n] = (char)k; host[n + 1] = 0; return 1; }
    if (k >= 'A' && k <= 'Z' && n + 1 < sizeof host) { host[n] = (char)(k + 32); host[n + 1] = 0; return 1; }
    return 0;
}

int installer_tick(void)
{
    static int last_pct = -1;
    if (step == S_RUN) {
        if (install_state == 2 || install_state == 3) { step = S_DONE; return 1; }
        if (install_pct != last_pct) { last_pct = install_pct; return 1; }
    }
    return 0;
}
