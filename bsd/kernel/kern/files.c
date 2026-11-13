/* Files: the Chrono file manager. Places on the left (home and its folders, the whole
 * computer), the folder's contents on the right, folders first. Double-click or Enter opens:
 * folders go in, web pages and pictures open in the browser, programs run, the rest opens in
 * Aether. New folder, rename, delete, copy / cut / paste, hidden files on and off.
 * Everything is done as the session's user: what their account may not touch, Files may not
 * either. Runs on the Chrono thread, like the other built-in apps. */
#include "kern.h"
#include "arch.h"
#include "gfx.h"
#include "fonts.h"
#include "apps.h"
#include "fs.h"
#include "vfs.h"
#include "cred.h"
#include "rs.h"
#include "chrono.h"

void chrono_run(const char *path);          /* chrono.c: start a program, detached */

#define MAXE 2048
#define ROW 28
#define SIDE 176
#define BAR 46

struct ent { char name[128]; int dir, link, exec; uint64_t size; uint32_t mtime; };
static struct ent ents[MAXE];
static int nents, sel = -1, scroll, show_hidden;
static char cwd[512], status[160];
static uint32_t listed_gen = 0xFFFFFFFF;
static char listed_dir[512];
static char hist[16][512]; static int nhist;
static char clip[512]; static int clip_cut;
static int prompt;                          /* 1 new folder, 2 rename, 3 confirm delete */
static char field[128];
static uint64_t last_click; static int last_idx = -1;
static int W_, H_;

struct rect { int x, y, w, h; };
static struct rect b_back, b_up, b_new, b_hidden, b_place[10];
static const char *const PLACES[] = { "Home", "Desktop", "Documents", "Downloads", "Pictures", "Music", "Videos", "Computer" };
#define NPLACES 8

static int in(const struct rect *r, int x, int y) { return x >= r->x && y >= r->y && x < r->x + r->w && y < r->y + r->h; }

static void place_path(int i, char *out, size_t n)
{
    if (i == 0) strlcpy(out, session_home(), n);
    else if (i == NPLACES - 1) strlcpy(out, "/", n);
    else ksnprintf(out, n, "%s/%s", session_home(), PLACES[i]);
}

static void join(const char *dir, const char *name, char *out, size_t n)
{
    if (!strcmp(dir, "/")) ksnprintf(out, n, "/%s", name);
    else ksnprintf(out, n, "%s/%s", dir, name);
}

static int cmp(const struct ent *a, const struct ent *b)
{
    if (a->dir != b->dir) return b->dir - a->dir;
    const char *p = a->name, *q = b->name;                     /* case-insensitive */
    while (*p && *q) {
        char x = *p >= 'A' && *p <= 'Z' ? (char)(*p + 32) : *p, y = *q >= 'A' && *q <= 'Z' ? (char)(*q + 32) : *q;
        if (x != y) return x - y;
        p++; q++;
    }
    return *p - *q;
}

static void relist(void)
{
    if (listed_gen == fs_generation() && !strcmp(listed_dir, cwd)) return;
    struct node *d;
    nents = 0;
    listed_gen = fs_generation();
    strlcpy(listed_dir, cwd, sizeof listed_dir);
    if (fs_lookup(0, cwd, 1, &d) < 0 || d->type != N_DIR) { ksnprintf(status, sizeof status, "%s isn't there any more", cwd); return; }
    if (!session_may(cwd, 5)) { ksnprintf(status, sizeof status, "You don't have permission to look in %s", cwd); return; }
    fs_lock();
    for (struct node *c = d->child; c && nents < MAXE; c = c->next) {
        if (!show_hidden && c->name[0] == '.') continue;
        struct ent *e = &ents[nents++];
        strlcpy(e->name, c->name, sizeof e->name);
        struct node *t = c;
        e->link = c->type == N_LNK;
        if (e->link) { char p[600]; join(cwd, c->name, p, sizeof p); fs_unlock(); if (fs_lookup(0, p, 1, &t) < 0) t = c; fs_lock(); }
        e->dir = t->type == N_DIR;
        e->size = t->type == N_REG ? t->size : 0;
        e->mtime = t->mtime;
        e->exec = t->type == N_REG && (t->mode & 0111);
    }
    fs_unlock();
    for (int i = 1; i < nents; i++) {                             /* insertion sort */
        struct ent k = ents[i]; int j = i;
        while (j > 0 && cmp(&ents[j - 1], &k) > 0) { ents[j] = ents[j - 1]; j--; }
        ents[j] = k;
    }
    if (sel >= nents) sel = nents - 1;
}

static void go(const char *path, int remember)
{
    char p[512];
    strlcpy(p, path, sizeof p);
    size_t l = strlen(p);
    while (l > 1 && p[l - 1] == '/') p[--l] = 0;
    struct node *n;
    if (fs_lookup(0, p, 1, &n) < 0 || n->type != N_DIR) { ksnprintf(status, sizeof status, "%s: no such folder", p); return; }
    if (!session_may(p, 5)) { ksnprintf(status, sizeof status, "You don't have permission to open %s", p); return; }
    if (remember && strcmp(p, cwd)) {
        if (nhist == 16) { memmove(hist[0], hist[1], sizeof hist[0] * 15); nhist--; }
        strlcpy(hist[nhist++], cwd, sizeof hist[0]);
    }
    strlcpy(cwd, p, sizeof cwd);
    sel = -1; scroll = 0; status[0] = 0; prompt = 0;
    listed_gen = 0xFFFFFFFF;
}

static void up(void)
{
    if (!strcmp(cwd, "/")) return;
    char p[512];
    strlcpy(p, cwd, sizeof p);
    char *s = p + strlen(p);
    while (s > p && *s != '/') s--;
    if (s == p) strlcpy(p, "/", sizeof p); else *s = 0;
    go(p, 1);
}

static int ends(const char *s, const char *suf) { size_t a = strlen(s), b = strlen(suf); if (a < b) return 0; for (size_t i = 0; i < b; i++) { char c = s[a - b + i]; if (c >= 'A' && c <= 'Z') c += 32; if (c != suf[i]) return 0; } return 1; }

static void open_entry(int i)
{
    if (i < 0 || i >= nents) return;
    char p[600];
    join(cwd, ents[i].name, p, sizeof p);
    if (ents[i].dir) { go(p, 1); return; }
    if (!session_may(p, 4)) { ksnprintf(status, sizeof status, "You don't have permission to open %s", ents[i].name); return; }
    const char *n = ents[i].name;
    if (ends(n, ".html") || ends(n, ".htm") || ends(n, ".png") || ends(n, ".jpg") || ends(n, ".jpeg") || ends(n, ".gif") ||
        ends(n, ".svg") || ends(n, ".webp") || ends(n, ".pdf")) {
        char u[620]; ksnprintf(u, sizeof u, "file://%s", p);
        chrono_open_url(u);
        return;
    }
    if (ents[i].exec) {                                         /* a program: is it a Linux one? */
        struct node *x;
        if (fs_lookup(0, p, 1, &x) == 0 && fs_load(x) == 0 && x->size > 4 && x->data &&
            ((x->data[0] == 0x7F && x->data[1] == 'E') || (x->data[0] == '#' && x->data[1] == '!'))) {
            chrono_run(p);
            ksnprintf(status, sizeof status, "Started %s", n);
            return;
        }
    }
    chrono_request_edit(p);                                     /* text, code, anything else: Aether */
}

/* copy a file or a folder (and what's in it) as the user */
static int copy_tree(const char *from, const char *to, int depth)
{
    struct node *s;
    if (depth > 32 || fs_lookup(0, from, 0, &s) < 0) return -1;
    if (!session_may(from, 4)) return -13;
    struct node *dir; char name[256];
    if (fs_lookup_parent(0, to, &dir, name, sizeof name) < 0) return -2;
    if (s->type == N_DIR) {
        struct node *d = fs_create(dir, name, N_DIR, s->mode & 07777);
        if (!d) return -12;
        char names[64][128]; int k = 0, r = 0;
        for (;;) {                                               /* in batches: the tree may change as we go */
            k = 0;
            fs_lock();
            for (struct node *c = s->child; c && k < 64; c = c->next) {
                char tp[600]; join(to, c->name, tp, sizeof tp);
                struct node *ex;
                fs_unlock();
                int exists = fs_lookup(0, tp, 0, &ex) == 0;
                fs_lock();
                if (exists) continue;
                strlcpy(names[k++], c->name, sizeof names[0]);
            }
            fs_unlock();
            if (!k) break;
            for (int i = 0; i < k && !r; i++) {
                char fp[600], tp[600];
                join(from, names[i], fp, sizeof fp); join(to, names[i], tp, sizeof tp);
                r = copy_tree(fp, tp, depth + 1);
            }
            if (r) return r;
        }
        return 0;
    }
    if (s->type == N_LNK) {
        struct node *l = fs_create(dir, name, N_LNK, 0777);
        if (!l) return -12;
        return fs_write(l, 0, s->data ? s->data : (const uint8_t *)"", s->size) < 0 ? -28 : 0;
    }
    if (s->type != N_REG) return 0;
    if (fs_load(s) < 0) return -5;
    struct node *f = fs_create(dir, name, N_REG, s->mode & 0777);
    if (!f) return -12;
    if (s->size && fs_write(f, 0, s->data, s->size) < 0) return -28;
    return 0;
}

static int remove_tree(const char *path, int depth)
{
    struct node *n, *dir; char name[256];
    if (depth > 32 || fs_lookup(0, path, 0, &n) < 0 || fs_lookup_parent(0, path, &dir, name, sizeof name) < 0) return -2;
    if (n->type == N_DIR) {
        for (;;) {
            fs_lock();
            struct node *c = n->child;
            char cn[256];
            if (c) strlcpy(cn, c->name, sizeof cn);
            fs_unlock();
            if (!c) break;
            char cp[600]; join(path, cn, cp, sizeof cp);
            int r = remove_tree(cp, depth + 1);
            if (r < 0) return r;
        }
    }
    return fs_unlink(dir, name, n->type == N_DIR);
}

static void unique(const char *dir, const char *name, char *out, size_t n)
{
    join(dir, name, out, n);
    struct node *x;
    for (int k = 2; fs_lookup(0, out, 0, &x) == 0 && k < 100; k++) {
        char nm[160];
        const char *dot = strrchr(name, '.');
        if (dot && dot != name) ksnprintf(nm, sizeof nm, "%.*s (%d)%s", (int)(dot - name), name, k, dot);
        else ksnprintf(nm, sizeof nm, "%s (%d)", name, k);
        join(dir, nm, out, n);
    }
}

static void paste(void)
{
    if (!clip[0]) return;
    if (!session_may(cwd, 3)) { ksnprintf(status, sizeof status, "You can't add things to this folder"); return; }
    const char *base = strrchr(clip, '/');
    base = base ? base + 1 : clip;
    char to[600];
    unique(cwd, base, to, sizeof to);
    int r;
    if (clip_cut) {
        struct node *od, *nd; char on[256], nn[256];
        if (fs_lookup_parent(0, clip, &od, on, sizeof on) < 0 || fs_lookup_parent(0, to, &nd, nn, sizeof nn) < 0) r = -13;
        else {
            r = fs_rename(od, on, nd, nn);
            if (r == 0) clip[0] = 0;
        }
    } else r = copy_tree(clip, to, 0);
    ksnprintf(status, sizeof status, r == 0 ? "%s %s" : "Couldn't paste %s (%s)", base,
              r == 0 ? (clip_cut ? "moved here" : "copied here") : r == -13 ? "permission denied" : r == -28 ? "no space" : "error");
}

static void selected_path(char *p, size_t n) { if (sel >= 0 && sel < nents) join(cwd, ents[sel].name, p, n); else p[0] = 0; }

static void finish_prompt(void)
{
    char p[600];
    if (prompt == 1 && field[0]) {                              /* new folder */
        struct node *d;
        if (!session_may(cwd, 3) || fs_lookup(0, cwd, 1, &d) < 0) ksnprintf(status, sizeof status, "You can't make folders here");
        else if (fs_child(d, field)) ksnprintf(status, sizeof status, "There's already something called %s", field);
        else if (!fs_create(d, field, N_DIR, 0755)) ksnprintf(status, sizeof status, "Couldn't make the folder");
    } else if (prompt == 2 && field[0] && sel >= 0) {           /* rename */
        struct node *d;
        if (!session_may(cwd, 3) || fs_lookup(0, cwd, 1, &d) < 0) ksnprintf(status, sizeof status, "You can't rename things here");
        else if (fs_child(d, field)) ksnprintf(status, sizeof status, "There's already something called %s", field);
        else fs_rename(d, ents[sel].name, d, field);
    } else if (prompt == 3 && sel >= 0) {                       /* delete */
        selected_path(p, sizeof p);
        if (!session_may(cwd, 3)) ksnprintf(status, sizeof status, "You can't delete things here");
        else {
            int r = remove_tree(p, 0);
            ksnprintf(status, sizeof status, r < 0 ? "Couldn't delete %s" : "Deleted %s", ents[sel].name);
        }
    }
    prompt = 0;
    listed_gen = 0xFFFFFFFF;
}

static void icon(struct canvas *c, int x, int y, const struct ent *e, const struct app_colors *k)
{
    if (e->dir) {
        uint32_t col = k->accent;
        gfx_round(c, x, y + 2, 8, 4, 1, col, 0);
        gfx_round(c, x, y + 4, 18, 13, 2, col, 0);
        gfx_fill(c, x + 1, y + 7, 16, 1, gfx_shade(col, 0.25f) | 0xff000000);
    } else {
        uint32_t col = e->exec ? 0xff5fb36b : ends(e->name, ".png") || ends(e->name, ".jpg") ? 0xffd0884c : k->dim;
        gfx_round(c, x + 2, y + 1, 14, 17, 2, col, 0);
        gfx_fill(c, x + 5, y + 7, 8, 1, k->bg); gfx_fill(c, x + 5, y + 10, 8, 1, k->bg); gfx_fill(c, x + 5, y + 13, 6, 1, k->bg);
    }
}

static void size_str(uint64_t b, char *o, size_t n)
{
    if (b < 1024) ksnprintf(o, n, "%lu bytes", (unsigned long)b);
    else if (b < 1024 * 1024) ksnprintf(o, n, "%lu.%lu kB", (unsigned long)(b / 1024), (unsigned long)(b % 1024 * 10 / 1024));
    else if (b < 1024ULL * 1024 * 1024) ksnprintf(o, n, "%lu.%lu MB", (unsigned long)(b >> 20), (unsigned long)((b & 0xFFFFF) * 10 >> 20));
    else ksnprintf(o, n, "%lu.%lu GB", (unsigned long)(b >> 30), (unsigned long)((b & 0x3FFFFFFF) * 10 >> 30));
}

static void date_str(uint32_t t, char *o, size_t n)
{
    int64_t z = (int64_t)(t / 86400) + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097), yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int y = (int)(yoe + era * 400);
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153, d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) y++;
    ksnprintf(o, n, "%04d-%02u-%02u %02u:%02u", y, m, d, (t / 3600) % 24, (t / 60) % 60);
}

static void button(struct canvas *c, const struct app_ctx *a, struct rect *r, int x, int y, int w, const char *label, int on)
{
    const struct app_colors *k = a->col;
    *r = (struct rect){ x - a->x, y - a->y, w, 28 };
    gfx_round(c, x, y, w, 28, 6, on ? k->accent : k->field, 0);
    gfx_text(c, &font_ui, x + (w - gfx_text_width(&font_ui, label)) / 2, y + 6, label, on ? k->accent_fg : k->fg);
}

const char *files_title(void)
{
    static char t[96];
    const char *b = strrchr(cwd, '/');
    ksnprintf(t, sizeof t, "Files — %s", !strcmp(cwd, session_home()) ? "Home" : b && b[1] ? b + 1 : "/");
    return t;
}

void files_paint(const struct app_ctx *a)
{
    struct canvas *c = a->c;
    const struct app_colors *k = a->col;
    int X = a->x, Y = a->y;
    W_ = a->w; H_ = a->h;
    if (!cwd[0]) go(session_home(), 0);
    relist();
    gfx_fill(c, X, Y, a->w, a->h, k->bg);
    /* toolbar */
    gfx_fill(c, X, Y, a->w, BAR, k->field);
    button(c, a, &b_back, X + 8, Y + 9, 34, "‹", 0);
    button(c, a, &b_up, X + 46, Y + 9, 34, "↑", 0);
    int pw = a->w - 84 - 230;
    gfx_round(c, X + 88, Y + 9, pw, 28, 6, k->bg, 0);
    gfx_text_fit(c, &font_ui, X + 98, Y + 15, pw - 20, cwd, k->fg);
    button(c, a, &b_new, X + a->w - 226, Y + 9, 110, "New folder", 0);
    button(c, a, &b_hidden, X + a->w - 110, Y + 9, 100, "Hidden files", show_hidden);
    /* places */
    gfx_fill(c, X, Y + BAR, SIDE, a->h - BAR, gfx_shade(k->bg, k->light ? -0.03f : 0.04f) | 0xff000000);
    for (int i = 0; i < NPLACES; i++) {
        int py = Y + BAR + 10 + i * 30 + (i == NPLACES - 1 ? 14 : 0);
        char pp[512]; place_path(i, pp, sizeof pp);
        int here = !strcmp(pp, cwd);
        b_place[i] = (struct rect){ 6, py - Y, SIDE - 12, 28 };
        if (here) gfx_round(c, X + 6, py, SIDE - 12, 28, 6, k->sel, 0);
        struct ent fake = { .dir = 1 };
        icon(c, X + 14, py + 4, &fake, k);
        gfx_text(c, &font_ui, X + 42, py + 6, PLACES[i], k->fg);
    }
    /* the list */
    int lx = X + SIDE, ly = Y + BAR, lw = a->w - SIDE, lh = a->h - BAR - 26;
    gfx_text(c, &font_bold, lx + 44, ly + 8, "Name", k->dim);
    gfx_text(c, &font_bold, lx + lw - 280, ly + 8, "Size", k->dim);
    gfx_text(c, &font_bold, lx + lw - 170, ly + 8, "Modified", k->dim);
    gfx_fill(c, lx, ly + 30, lw, 1, gfx_mix(k->fg, k->bg, 0.85f));
    gfx_clip(c, lx, ly + 32, lw, lh - 32);
    int vis = (lh - 32) / ROW;
    if (sel >= 0 && sel < scroll) scroll = sel;
    if (sel >= scroll + vis) scroll = sel - vis + 1;
    if (scroll > nents - vis) scroll = nents - vis;
    if (scroll < 0) scroll = 0;
    for (int i = scroll; i < nents && i < scroll + vis + 1; i++) {
        int ry = ly + 32 + (i - scroll) * ROW;
        struct ent *e = &ents[i];
        if (i == sel) gfx_round(c, lx + 6, ry + 1, lw - 12, ROW - 2, 6, k->sel, 0);
        icon(c, lx + 16, ry + 4, e, k);
        char nm[160];
        ksnprintf(nm, sizeof nm, "%s%s", e->name, e->link ? "  →" : "");
        if (prompt == 2 && i == sel) {
            gfx_round(c, lx + 40, ry + 2, lw - 340, ROW - 4, 4, k->field, 0);
            gfx_round_frame(c, lx + 40, ry + 2, lw - 340, ROW - 4, 4, k->accent);
            int tw = gfx_text(c, &font_ui, lx + 46, ry + 6, field, k->fg);
            if (!(a->blink & 1)) gfx_fill(c, lx + 47 + tw, ry + 6, 1, 16, k->fg);
        } else gfx_text_fit(c, &font_ui, lx + 44, ry + 6, lw - 340, nm, k->fg);
        char sz[32], dt[32];
        if (e->dir) strlcpy(sz, "—", sizeof sz); else size_str(e->size, sz, sizeof sz);
        date_str(e->mtime, dt, sizeof dt);
        gfx_text(c, &font_ui, lx + lw - 280, ry + 6, sz, k->dim);
        gfx_text(c, &font_ui, lx + lw - 170, ry + 6, dt, k->dim);
    }
    if (!nents) gfx_text(c, &font_ui, lx + 44, ly + 44, status[0] ? status : "This folder is empty", k->dim);
    gfx_noclip(c);
    /* status bar / prompts */
    int sy = Y + a->h - 26;
    gfx_fill(c, lx, sy, lw, 26, k->field);
    char line[220];
    if (prompt == 1) {
        ksnprintf(line, sizeof line, "New folder name: %s", field);
        int tw = gfx_text(c, &font_ui, lx + 10, sy + 5, line, k->fg);
        if (!(a->blink & 1)) gfx_fill(c, lx + 11 + tw, sy + 5, 1, 16, k->fg);
    } else if (prompt == 3 && sel >= 0) {
        ksnprintf(line, sizeof line, "Delete %s%s for good? Enter: delete · Esc: keep it", ents[sel].name, ents[sel].dir ? " and everything in it" : "");
        gfx_text_fit(c, &font_bold, lx + 10, sy + 5, lw - 20, line, k->danger);
    } else {
        if (status[0]) strlcpy(line, status, sizeof line);
        else ksnprintf(line, sizeof line, "%d item%s · Enter opens · F2 renames · Del deletes · Ctrl+C / X / V", nents, nents == 1 ? "" : "s");
        gfx_text_fit(c, &font_ui, lx + 10, sy + 5, lw - 20, line, k->dim);
    }
}

int files_click(int x, int y, int w, int h)
{
    (void)w; (void)h;
    if (prompt == 2 || prompt == 1) finish_prompt();
    if (prompt == 3) prompt = 0;
    if (in(&b_back, x, y)) { if (nhist) go(hist[--nhist], 0); return 1; }
    if (in(&b_up, x, y)) { up(); return 1; }
    if (in(&b_new, x, y)) { prompt = 1; field[0] = 0; status[0] = 0; return 1; }
    if (in(&b_hidden, x, y)) { show_hidden = !show_hidden; listed_gen = 0xFFFFFFFF; return 1; }
    for (int i = 0; i < NPLACES; i++) if (in(&b_place[i], x, y)) {
        char p[512]; place_path(i, p, sizeof p);
        struct node *n;
        if (i > 0 && i < NPLACES - 1 && fs_lookup(0, p, 1, &n) < 0) {   /* make the usual folders when first wanted */
            struct node *home;
            if (fs_lookup(0, session_home(), 1, &home) == 0) fs_create(home, PLACES[i], N_DIR, 0755);
        }
        go(p, 1);
        return 1;
    }
    if (x >= SIDE && y >= BAR + 32 && y < H_ - 26) {
        int i = scroll + (y - BAR - 32) / ROW;
        if (i >= nents) { sel = -1; return 1; }
        uint64_t now = kernel_ticks();
        if (i == last_idx && now - last_click < AUXO_HZ / 2) { last_idx = -1; open_entry(i); return 1; }
        last_idx = i; last_click = now;
        sel = i; status[0] = 0;
    }
    return 1;
}

int files_wheel(int dz) { scroll += dz > 0 ? -3 : 3; if (scroll < 0) scroll = 0; return 1; }

int files_key(int k)
{
    if (prompt == 1 || prompt == 2) {
        size_t n = strlen(field);
        if (k == '\r' || k == '\n') { finish_prompt(); return 1; }
        if (k == 27) { prompt = 0; return 1; }
        if (k == '\b' || k == 0x7f) { if (n) field[n - 1] = 0; return 1; }
        if (k >= 0x20 && k < 0x7f && k != '/' && n + 1 < sizeof field) { field[n] = (char)k; field[n + 1] = 0; }
        return 1;
    }
    if (prompt == 3) { if (k == '\r' || k == '\n') finish_prompt(); else if (k == 27) prompt = 0; return 1; }
    int ctrl = input_ctrl();                                    /* (Ctrl+letters arrive as control characters) */
    char p[600];
    if (k == 3) { selected_path(p, sizeof p); if (p[0]) { strlcpy(clip, p, sizeof clip); clip_cut = 0; ksnprintf(status, sizeof status, "Copied %s: Ctrl+V pastes it", ents[sel].name); } return 1; }
    if (k == 24) { selected_path(p, sizeof p); if (p[0]) { strlcpy(clip, p, sizeof clip); clip_cut = 1; ksnprintf(status, sizeof status, "Cut %s: Ctrl+V moves it", ents[sel].name); } return 1; }
    if (k == 22) { paste(); listed_gen = 0xFFFFFFFF; return 1; }
    if (k == KEY_DOWN) { if (sel < nents - 1) sel++; return 1; }
    if (k == KEY_UP) { if (sel > 0) sel--; else if (nents) sel = 0; return 1; }
    if (k == KEY_HOME) { sel = nents ? 0 : -1; return 1; }
    if (k == KEY_END) { sel = nents - 1; return 1; }
    if (k == KEY_PGDN) { sel += 10; if (sel >= nents) sel = nents - 1; return 1; }
    if (k == KEY_PGUP) { sel -= 10; if (sel < 0) sel = nents ? 0 : -1; return 1; }
    if (k == '\r' || k == '\n') { open_entry(sel); return 1; }
    if (k == '\b' || k == 0x7f || k == KEY_LEFT) { up(); return 1; }
    if (k == KEY_RIGHT) { if (sel >= 0 && ents[sel].dir) open_entry(sel); return 1; }
    if (k == KEY_DEL && sel >= 0) { prompt = 3; return 1; }
    if (k == KEY_F2 && sel >= 0) { prompt = 2; strlcpy(field, ents[sel].name, sizeof field); return 1; }
    if (k > 0x20 && k < 0x7f && !ctrl) {                         /* type to jump */
        char c = k >= 'A' && k <= 'Z' ? (char)(k + 32) : (char)k;
        for (int j = 1; j <= nents; j++) {
            int i = (sel + j) % (nents ? nents : 1);
            char f = ents[i].name[0] >= 'A' && ents[i].name[0] <= 'Z' ? (char)(ents[i].name[0] + 32) : ents[i].name[0];
            if (f == c) { sel = i; break; }
        }
        return 1;
    }
    return 0;
}

void files_open(const char *path) { go(path, 1); }
