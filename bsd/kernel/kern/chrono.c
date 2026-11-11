/* Chrono on AuxoBSD: the Chrono desktop (github.com/rxvy-dev/chrono), drawn by the kernel.
 *
 * Chrono's themes are compiled in from its own .theme files (include/chrono_themes.h). The
 * wallpapers follow Chrono's common/paint.c pattern by pattern, the window frames follow its
 * decoration styles (flat, bevel, platinum, pixel, tab, motif, cube, meadow, pinstripe, glass,
 * neon) and button shapes, and the panel its styles and layout: app menu, running apps, clock.
 *
 * Runs as the "chrono" kernel thread, the only thread that uses SSE (see Makefile GUI_CFLAGS).
 * Other threads talk to it through chrono_request_*(), which only set flags. */
#include "kern.h"
#include "arch.h"
#include "rs.h"
#include "auxo.h"
#include "gfx.h"
#include "fonts.h"
#include "chrono.h"
#include "chrono_themes.h"
#include "apps.h"
#include "vfs.h"

_Static_assert(CHRONO_THEMES == CHRONO_THEME_COUNT, "update CHRONO_THEMES in chrono.h");

#define MAXWIN 8
#define TERM_COLS 88
#define TERM_ROWS 28

/* ── state ──────────────────────────────────────────────────────────── */
static struct canvas scr, wallc;
static int W, H, running;
static int cur = CHRONO_THEME_DEFAULT;
static const struct chrono_theme *T;
static volatile int req_theme = -1, req_open = -1, req_redraw, req_edit, req_url;
static char req_path[160], req_link[512];
static uint16_t term_cells[TERM_COLS * TERM_ROWS];

struct win {
    int used, app, x, y, w, h;          /* x,y: outer top-left; w,h: client size */
    int minimized, maximized, sx, sy;   /* sx,sy: position before maximizing */
    int scroll, sel, sw, sh;            /* sw,sh: size before maximizing */
    const char *title;
};
static struct win wins[MAXWIN];
static int zorder[MAXWIN], nz;            /* bottom → top */
static int focus = -1;

static int mx, my, buttons, menu_open, drag = -1, drag_dx, drag_dy, hover_item = -1, resize = -1;
static struct app_colors colors;
static uint32_t term_gen, last_minute = 99, blink;

static const char *const APP_TITLES[] = { "Terminal", "Settings", "About AuxoBSD", "Browser", "Aether", "Install AuxoBSD" };

/* ── requests from other threads (no floating point there) ──────────── */
int  chrono_running(void) { return running; }
int  chrono_current_theme(void) { return cur; }
void chrono_request_theme(int i) { if (i >= 0 && i < CHRONO_THEME_COUNT) req_theme = i; }
void chrono_request_open(int app) { if (app >= 0 && app < APP_COUNT) req_open = app; }
void chrono_request_redraw(void) { req_redraw = 1; }
void chrono_request_edit(const char *p) { strlcpy(req_path, p, sizeof req_path); req_edit = 1; }
void chrono_request_url(const char *u) { strlcpy(req_link, u, sizeof req_link); req_url = 1; }

const char *chrono_theme_id(int i) { return i >= 0 && i < CHRONO_THEME_COUNT ? chrono_themes[i].id : 0; }
const char *chrono_theme_name(int i) { return i >= 0 && i < CHRONO_THEME_COUNT ? chrono_themes[i].name : 0; }
int chrono_theme_retro(int i) { return i >= 0 && i < CHRONO_THEME_COUNT && chrono_themes[i].retro; }

int chrono_theme_find(const char *id)
{
    for (int i = 0; i < CHRONO_THEME_COUNT; i++)
        if (!strcmp(chrono_themes[i].id, id)) return i;
    return -1;
}

/* ── wallpapers: Chrono's common/paint.c desktop patterns ───────────── */
static unsigned rng;
static float rnd(void) { rng = rng * 1103515245u + 12345u; return (float)((rng >> 8) & 0xFFFFFF) / (float)0x1000000; }

static void paint_wallpaper(struct canvas *c)
{
    uint32_t a = T->c[C_DESKTOP1], b = T->c[C_DESKTOP2], acc = T->c[C_ACCENT];
    int w = c->w, h = c->h;
    float sc = (float)(w > h ? w : h) / 1920.0f;
    if (sc < 1) sc = 1;
    switch (T->wallpaper) {
    case WP_SOLID: gfx_fill(c, 0, 0, w, h, a | 0xff000000); break;
    case WP_GRADIENT: gfx_vgrad(c, 0, 0, w, h, a, b); break;
    case WP_WAVES:
        gfx_vgrad(c, 0, 0, w, h, a, gfx_mix(a, b, 0.35f));
        for (int i = 0; i < 6; i++) {
            float base = h * (0.38f + i * 0.11f), amp = h * (0.05f - i * 0.004f), ph = i * 1.7f;
            uint32_t col = gfx_mix(gfx_mix(a, b, 0.25f + i * 0.14f), acc, i == 5 ? 0.12f : 0.04f * i) | 0xff000000;
            for (int x = 0; x < w; x++) {
                float y = base + gfx_sin((float)x / ((float)w / (2.2f + i * 0.35f)) * GFX_PI + ph) * amp
                          + gfx_sin((float)x / ((float)w / 7.0f) + ph * 2) * amp * 0.25f;
                gfx_column(c, x, y, h, col);
            }
        }
        break;
    case WP_TOPO:
        gfx_vgrad(c, 0, 0, w, h, a, gfx_mix(a, b, 0.5f));
        for (int k = 1; k < 22; k++) {
            float lv = k / 22.0f;
            uint32_t col = with_alpha(gfx_mix(b, acc, k % 5 == 0 ? 0.5f : 0.0f), k % 5 == 0 ? 140 : 72);
            for (int ring = 0; ring < 3; ring++) {
                float cx = w * (0.25f + 0.3f * ring), cy = h * (0.35f + 0.2f * ((ring + 1) % 2));
                float R = (w > h ? w : h) * (0.06f + lv * 0.75f) * (0.6f + 0.25f * ring);
                float px = 0, py = 0;
                for (int st = 0; st <= 120; st++) {
                    float th = st / 120.0f * 2 * GFX_PI;
                    float r = R * (1 + 0.12f * gfx_sin(3 * th + ring + k * 0.2f) + 0.06f * gfx_sin(7 * th - k * 0.3f));
                    float x = cx + gfx_cos(th) * r, y = cy + gfx_sin(th) * r * 0.75f;
                    if (st) gfx_line(c, px, py, x, y, 1.2f * sc, col);
                    px = x; py = y;
                }
            }
        }
        break;
    case WP_BLOBS: case WP_MATERIAL: {
        int crisp = T->wallpaper == WP_MATERIAL;
        gfx_fill(c, 0, 0, w, h, a | 0xff000000);
        static const float bl[5][4] = { {0.15f, 0.2f, 0.42f, 0.9f}, {0.85f, 0.15f, 0.32f, 0.5f}, {0.7f, 0.85f, 0.48f, 0.75f},
                                        {0.25f, 0.95f, 0.3f, 0.35f}, {0.5f, 0.45f, 0.2f, 0.15f} };
        for (int i = 0; i < 5; i++) {
            uint32_t col = gfx_mix(gfx_mix(a, b, bl[i][3]), acc, crisp ? 0.25f * (i % 2) : (i % 3 == 0 ? 0.18f : 0));
            float R = bl[i][2] * (w > h ? w : h);
            if (crisp) gfx_circle(c, bl[i][0] * w, bl[i][1] * h, R, with_alpha(col, 230));
            else gfx_radial(c, bl[i][0] * w, bl[i][1] * h, R, with_alpha(col, 217), 0);
        }
        break;
    }
    case WP_SCANLINES: {
        gfx_fill(c, 0, 0, w, h, a | 0xff000000);
        for (int y = 0; y < h; y += 3) gfx_fill(c, 0, y, w, 1, with_alpha(b, 90));
        float r0 = (w < h ? w : h) * 0.3f, r1 = (w > h ? w : h) * 0.75f;
        for (int y = 0; y < h; y += 2)                 /* vignette */
            for (int x = 0; x < w; x += 2) {
                float dx = x - w / 2.0f, dy = y - h / 2.0f, d = gfx_sqrt(dx * dx + dy * dy);
                if (d <= r0) continue;
                float t = (d - r0) / (r1 - r0);
                int al = (int)(153 * (t > 1 ? 1 : t));
                gfx_fill(c, x, y, 2, 2, (uint32_t)al << 24);
            }
        break;
    }
    case WP_CIRCUIT: {
        gfx_vgrad(c, 0, 0, w, h, a, b);
        rng = 11;
        float g = 32 * sc;
        for (int i = 0; i < 38; i++) {
            float x = gfx_floor(rnd() * w / g) * g, y = gfx_floor(rnd() * h / g) * g;
            uint32_t col = rnd() < 0.5f ? acc : gfx_mix(acc, b, 0.6f);
            col = with_alpha(col, (int)(255 * (0.18f + rnd() * 0.35f)));
            for (int s2 = 0; s2 < 5; s2++) {
                float d = g * (1 + gfx_floor(rnd() * 4)), nx = x, ny = y;
                if (rnd() < 0.5f) nx += rnd() < 0.5f ? d : -d; else ny += rnd() < 0.5f ? d : -d;
                gfx_line(c, x, y, nx, ny, 1.5f * sc, col);
                x = nx; y = ny;
            }
            gfx_circle(c, x, y, 3 * sc, col);
        }
        break;
    }
    case WP_DUNES:
        gfx_vgrad(c, 0, 0, w, (int)(h * 0.6f), b, gfx_mix(b, a, 0.3f));
        gfx_fill(c, 0, (int)(h * 0.6f), w, h - (int)(h * 0.6f), gfx_mix(b, a, 0.3f) | 0xff000000);
        for (int i = 0; i < 4; i++) {
            uint32_t col = gfx_mix(a, gfx_mix(b, acc, 0.15f), 0.15f + i * 0.12f) | 0xff000000;
            float base = h * (0.55f + i * 0.12f);
            /* the cubic curve_to of paint.c, sampled per column */
            float p0 = base, p1 = base - h * 0.12f, p2 = base + h * 0.08f, p3 = base - h * (0.05f + 0.03f * i);
            for (int x = 0; x < w; x++) {
                float t = (float)x / (float)w, u = 1 - t;
                float y = u * u * u * p0 + 3 * u * u * t * p1 + 3 * u * t * t * p2 + t * t * t * p3;
                gfx_column(c, x, y, h, col);
            }
        }
        break;
    case WP_RINGS:
        gfx_vgrad(c, 0, 0, w, h, a, gfx_mix(a, b, 0.4f));
        for (int i = 1; i < 26; i++) {
            uint32_t col = with_alpha(gfx_mix(b, acc, (i % 4 == 0) ? 0.6f : 0.1f), (int)(255 * (0.10f + 0.25f * (i % 4 == 0))));
            gfx_ring(c, w * 0.82f, h * 0.92f, i * 58 * sc, 2 * sc, col);
        }
        break;
    case WP_CHECKER:
        gfx_fill(c, 0, 0, w, h, a | 0xff000000);
        for (int y = 0; y < h; y += 16)
            for (int x = (y / 16) % 2 ? 16 : 0; x < w; x += 32) gfx_fill(c, x, y, 16, 16, b | 0xff000000);
        break;
    case WP_PINSTRIPE:
        gfx_fill(c, 0, 0, w, h, a | 0xff000000);
        for (int y = 0; y < h; y += 4) gfx_fill(c, 0, y, w, 2, b | 0xff000000);
        break;
    case WP_DOTS: case WP_PLATINUM: {
        int s = T->wallpaper == WP_PLATINUM ? 4 : 12, d = s == 4 ? 1 : 2;
        gfx_fill(c, 0, 0, w, h, a | 0xff000000);
        for (int y = 0; y < h; y += s)
            for (int x = (y / s) % 2 ? s / 2 : 0; x < w; x += s) gfx_fill(c, x, y, d, d, b | 0xff000000);
        break;
    }
    case WP_WEAVE:
        gfx_fill(c, 0, 0, w, h, a | 0xff000000);
        for (int y = 0; y < h; y += 8)
            for (int x = 0; x < w; x += 8) {
                if (((x / 8) + (y / 8)) % 2) gfx_fill(c, x, y + 3, 8, 2, b | 0xff000000);
                else gfx_fill(c, x + 3, y, 2, 8, b | 0xff000000);
            }
        break;
    case WP_STARS: {
        gfx_vgrad(c, 0, 0, w, h, a, b);
        rng = 7;
        int n = w * h / 2600;
        for (int i = 0; i < n; i++) {
            float x = rnd() * w, y = rnd() * h, r = 0.4f + rnd() * rnd() * 1.6f;
            gfx_circle(c, x, y, r, with_alpha(0xffffff, (int)(255 * (0.25f + rnd() * 0.7f))));
        }
        break;
    }
    case WP_GRID: {
        int hz = (int)(h * 0.62f);
        gfx_vgrad(c, 0, 0, w, hz, a, b);
        float sr = (w < h ? w : h) * 0.2f;
        /* the striped sun: a half disc, vertical gradient, cut by horizontal bands */
        for (int y = (int)(hz - sr); y < hz; y++) {
            float dy = hz - y, half = gfx_sqrt(sr * sr - dy * dy);
            uint32_t col = gfx_mix(T->c[C_TITLE_ACTIVE2], T->c[C_ACCENT], (y - (hz - sr)) / sr) | 0xff000000;
            gfx_fill(c, (int)(w / 2.0f - half), y, (int)(2 * half), 1, col);
        }
        for (int i = 0; i < 6; i++) {
            float y = hz - sr * 0.45f + i * sr * 0.09f;
            gfx_fill(c, (int)(w / 2.0f - sr), (int)y, (int)(2 * sr), 2 + (int)(i * 0.8f), a | 0xff000000);
        }
        gfx_fill(c, 0, hz, w, h - hz, 0xff0a0014);
        uint32_t line = with_alpha(acc, 217);
        for (int i = -20; i <= 20; i++) gfx_line(c, w / 2.0f + i * w * 0.02f, hz, w / 2.0f + i * w * 0.12f, h, 1.5f, line);
        for (float k = 0.02f; k < 1.0f; k *= 1.45f) gfx_fill(c, 0, (int)(hz + (h - hz) * k), w, 1, line);
        break;
    }
    case WP_HILLS: {
        gfx_vgrad(c, 0, 0, w, (int)(h * 0.7f), a, gfx_mix(a, 0xffffffff, 0.55f));
        gfx_fill(c, 0, (int)(h * 0.7f), w, h, gfx_mix(a, 0xffffffff, 0.55f) | 0xff000000);
        rng = 3;
        for (int i = 0; i < 9; i++) {
            float cx = rnd() * w, cy = rnd() * h * 0.35f, r = (0.03f + rnd() * 0.05f) * w;
            gfx_radial(c, cx, cy, r, 0x8cffffff, 0);
        }
        float p0 = h * 0.72f, p1 = h * 0.5f, p2 = h * 0.58f, p3 = h * 0.66f;
        uint32_t top = gfx_mix(b, 0xffffff99, 0.25f), bot = gfx_mix(b, 0xff000000, 0.3f);
        for (int x = 0; x < w; x++) {
            float t = (float)x / (float)w, u = 1 - t;
            float y = u * u * u * p0 + 3 * u * u * t * p1 + 3 * u * t * t * p2 + t * t * t * p3;
            for (int yy = (int)y; yy < h; yy++)
                gfx_pixel(c, x, yy, gfx_mix(top, bot, (float)(yy - h * 0.55f) / (h * 0.45f)) | 0xff000000);
        }
        break;
    }
    case WP_AURORA: {
        gfx_vgrad(c, 0, 0, w, h, a, b);
        uint32_t cols[4] = { T->c[C_ACCENT], T->c[C_TITLE_ACTIVE2], T->c[C_CLOSE_BG], T->c[C_SELECTION] };
        static const float pos[4][3] = { {0.18f, 0.25f, 0.55f}, {0.82f, 0.2f, 0.45f}, {0.6f, 0.85f, 0.6f}, {0.1f, 0.9f, 0.4f} };
        for (int i = 0; i < 4; i++)
            gfx_radial(c, pos[i][0] * w, pos[i][1] * h, pos[i][2] * (w > h ? w : h), with_alpha(cols[i], 140), 1);
        break;
    }
    default: gfx_vgrad(c, 0, 0, w, h, a, b); break;
    }
}

/* ── window geometry, after chrono_decor_geometry() ─────────────────── */
static int inset(void)
{
    int d = T->decor;
    return d == DECOR_BEVEL || d == DECOR_MOTIF || d == DECOR_PLATINUM || d == DECOR_PIXEL || d == DECOR_CUBE;
}
static int border(void) { return T->border_width; }
static int top_h(void) { return inset() ? border() + T->title_height + (T->decor == DECOR_BEVEL ? 1 : 0) : T->title_height; }
static int outer_w(const struct win *w) { return w->w + 2 * border(); }
static int outer_h(const struct win *w) { return w->h + top_h() + border(); }
static int client_x(const struct win *w) { return w->x + border(); }
static int client_y(const struct win *w) { return w->y + top_h(); }

struct btn { int kind, x, y, w, h; };

static int button_rects(const struct win *wn, struct btn *out)
{
    int title_y = inset() ? border() : 0, th = T->title_height;
    int bs = T->button_size;
    if (T->decor == DECOR_BEVEL || T->decor == DECOR_MOTIF) bs = th - 4;
    if (bs > th) bs = th;
    int bw = T->decor == DECOR_BEVEL ? bs + 2 : bs;
    int by = wn->y + title_y + (th - bs) / 2;
    int pad = inset() ? 2 : (th - bs) / 2;
    int gap = T->button_shape == BSHAPE_GEL || T->button_shape == BSHAPE_CIRCLE ? 8 : (T->decor == DECOR_BEVEL ? 0 : 4);
    if (T->decor == DECOR_PIXEL || T->decor == DECOR_MOTIF) gap = 0;
    int n = 0, x = wn->x + border() + pad;
    for (int i = 0; i < 4 && T->buttons_left[i]; i++) {
        out[n++] = (struct btn){ T->buttons_left[i], x, by, bw, bs };
        x += bw + gap;
    }
    int nr = 0;
    while (nr < 4 && T->buttons_right[nr]) nr++;
    int rx = wn->x + outer_w(wn) - border() - pad;
    for (int i = nr - 1; i >= 0; i--) {
        if (T->decor == DECOR_BEVEL && i < nr - 1 && T->buttons_right[i + 1] == BTN_CLOSE) rx -= 2;
        rx -= bw;
        out[n++] = (struct btn){ T->buttons_right[i], rx, by, bw, bs };
        rx -= gap;
    }
    return n;
}

/* ── frames ─────────────────────────────────────────────────────────── */
static void glyph_mark(struct canvas *c, int kind, int x, int y, int w, int h, uint32_t col)
{
    float cx = x + w / 2.0f, cy = y + h / 2.0f, s = (w < h ? w : h) * 0.28f;
    if (s < 3) s = 3;
    if (kind == BTN_CLOSE) {
        gfx_line(c, cx - s, cy - s, cx + s, cy + s, 1.6f, col);
        gfx_line(c, cx - s, cy + s, cx + s, cy - s, 1.6f, col);
    } else if (kind == BTN_MAXIMIZE) {
        gfx_frame(c, (int)(cx - s), (int)(cy - s), (int)(2 * s + 1), (int)(2 * s + 1), 1, col);
        gfx_fill(c, (int)(cx - s), (int)(cy - s), (int)(2 * s + 1), 2, col);
    } else if (kind == BTN_MINIMIZE) {
        gfx_fill(c, (int)(cx - s), (int)(cy + s - 1), (int)(2 * s + 1), 2, col);
    } else {
        for (int i = -1; i <= 1; i++) gfx_fill(c, (int)(cx - s), (int)(cy + i * s * 0.7f), (int)(2 * s + 1), 1, col);
    }
}

static void paint_button(struct canvas *c, const struct btn *b, int active)
{
    uint32_t bg = T->c[C_BUTTON_BG], fg = T->c[C_BUTTON_FG];
    if (b->kind == BTN_CLOSE && T->button_shape != BSHAPE_BEVEL) bg = T->c[C_CLOSE_BG];
    switch (T->button_shape) {
    case BSHAPE_BEVEL:
        gfx_fill(c, b->x, b->y, b->w, b->h, bg);
        gfx_bevel(c, b->x, b->y, b->w, b->h, T->c[C_BEVEL_LIGHT], T->c[C_BEVEL_DARK], T->c[C_BEVEL_SHADOW], 0);
        glyph_mark(c, b->kind, b->x, b->y, b->w, b->h, fg);
        break;
    case BSHAPE_CIRCLE:
        gfx_circle(c, b->x + b->w / 2.0f, b->y + b->h / 2.0f, b->h / 2.0f, active ? bg : gfx_shade(bg, -0.25f));
        if (active) glyph_mark(c, b->kind, b->x + 2, b->y + 2, b->w - 4, b->h - 4, b->kind == BTN_CLOSE ? 0xffffffff : fg);
        break;
    case BSHAPE_GEL: {
        static const uint32_t traffic[5] = { 0, 0xffff5f57, 0xff28c840, 0xfffebc2e, 0xff8e8e93 };
        uint32_t g = active ? traffic[b->kind] : 0xffb8b8b8;
        float r = b->h / 2.0f, cx = b->x + b->w / 2.0f, cy = b->y + b->h / 2.0f;
        gfx_circle(c, cx, cy, r, gfx_shade(g, -0.3f));
        gfx_circle(c, cx, cy, r - 1, g);
        gfx_circle(c, cx, cy - r * 0.35f, r * 0.5f, 0x66ffffff);
        break;
    }
    case BSHAPE_SQUARE:
        gfx_fill(c, b->x, b->y, b->w, b->h, bg);
        glyph_mark(c, b->kind, b->x, b->y, b->w, b->h, b->kind == BTN_CLOSE ? 0xffffffff : fg);
        break;
    case BSHAPE_ROUNDRECT:
        gfx_round(c, b->x, b->y, b->w, b->h, 4, bg, 0);
        glyph_mark(c, b->kind, b->x, b->y, b->w, b->h, b->kind == BTN_CLOSE ? 0xffffffff : fg);
        break;
    default:
        glyph_mark(c, b->kind, b->x, b->y, b->w, b->h, active ? T->c[C_TITLE_ACTIVE_FG] : T->c[C_TITLE_INACTIVE_FG]);
    }
}

static void paint_frame(struct canvas *c, const struct win *wn, int active)
{
    int X = wn->x, Y = wn->y, OW = outer_w(wn), OH = outer_h(wn), B = border(), TH = T->title_height;
    uint32_t ta = T->c[active ? C_TITLE_ACTIVE : C_TITLE_INACTIVE], ta2 = T->c[active ? C_TITLE_ACTIVE2 : C_TITLE_INACTIVE2];
    uint32_t tf = T->c[active ? C_TITLE_ACTIVE_FG : C_TITLE_INACTIVE_FG], bc = T->c[active ? C_BORDER_ACTIVE : C_BORDER_INACTIVE];
    int r = inset() ? 0 : T->corner_radius;
    int title_y = Y + (inset() ? B : 0), text_left;
    const struct font *tfont = T->title_bold ? &font_bold : &font_ui;

    if (T->shadow && !wn->maximized) gfx_shadow(c, X, Y, OW, OH, r, T->shadow_size, T->shadow_alpha * (active ? 1 : 0.6f));
    else if (!T->shadow && inset() && T->retro && !wn->maximized) gfx_fill(c, X + 4, Y + 4, OW, OH, 0x40000000);

    switch (T->decor) {
    case DECOR_BEVEL: case DECOR_MOTIF: case DECOR_CUBE: case DECOR_PLATINUM: {
        uint32_t frame = T->decor == DECOR_MOTIF ? bc : T->c[C_WINDOW_BG];
        gfx_fill(c, X, Y, OW, OH, frame | 0xff000000);
        gfx_bevel(c, X, Y, OW, OH, T->c[C_BEVEL_LIGHT], T->c[C_BEVEL_DARK], T->c[C_BEVEL_SHADOW], 0);
        if (T->decor == DECOR_PLATINUM) {
            gfx_fill(c, X + B, title_y, OW - 2 * B, TH, ta);
            for (int y = title_y + 3; y < title_y + TH - 3; y += 2) {
                gfx_fill(c, X + B + 4, y, OW - 2 * B - 8, 1, T->c[C_BEVEL_LIGHT]);
                gfx_fill(c, X + B + 4, y + 1, OW - 2 * B - 8, 1, T->c[C_BEVEL_DARK]);
            }
        } else if (T->decor == DECOR_CUBE) {
            gfx_fill(c, X + B, title_y, OW - 2 * B, TH, 0xff000000 | ta);
        } else {
            gfx_hgrad(c, X + B, title_y, OW - 2 * B, TH, ta, ta2);
        }
        if (T->decor != DECOR_PLATINUM)                /* sunken client edge */
            gfx_bevel(c, client_x(wn) - 1, client_y(wn) - 1, wn->w + 2, wn->h + 2, T->c[C_BEVEL_LIGHT],
                      T->c[C_BEVEL_DARK], T->c[C_BEVEL_SHADOW], 1);
        break;
    }
    case DECOR_PIXEL:
        gfx_fill(c, X, Y, OW, OH, T->c[C_WINDOW_BG] | 0xff000000);
        gfx_frame(c, X, Y, OW, OH, 1 > B ? 1 : B, bc | 0xff000000);
        gfx_fill(c, X + B, title_y, OW - 2 * B, TH, ta);
        gfx_fill(c, X + B, title_y + TH, OW - 2 * B, 1, bc | 0xff000000);
        break;
    default: {                                         /* flat, tab, meadow, pinstripe, glass, neon */
        int tw = OW;
        if (T->decor == DECOR_TAB) {
            tw = gfx_text_width(tfont, wn->title) + 24 + 3 * (T->button_size + 8);
            if (tw < 120) tw = 120;
            if (tw > OW) tw = OW;
            gfx_round(c, X, Y + TH, OW, OH - TH, 0, T->c[C_WINDOW_BG] | 0xff000000, 0);
            gfx_round_vgrad(c, X, Y, tw, TH + r, r, ta, ta2, 1);
        } else {
            gfx_round(c, X, Y, OW, OH, r, T->c[C_WINDOW_BG] | 0xff000000, 0);
            gfx_round_vgrad(c, X, Y, OW, TH + (r > 0 ? r : 0), r, ta, ta2, 1);
            gfx_fill(c, X, Y + TH, OW, r > 0 ? r : 0, T->c[C_WINDOW_BG] | 0xff000000);
        }
        if (T->decor == DECOR_MEADOW || T->panel_style == PANEL_GLOSS)
            gfx_round_vgrad(c, X + 1, Y + 1, tw - 2, TH / 2, r, 0x55ffffff, 0x10ffffff, 1);
        if (T->decor == DECOR_PINSTRIPE)
            for (int y = Y + 2; y < Y + TH - 1; y += 2) gfx_fill(c, X + r, y, tw - 2 * r, 1, 0x18ffffff);
        if (T->decor == DECOR_GLASS) gfx_fill(c, X + r, Y, tw - 2 * r, 1, 0x60ffffff);
        if (T->decor == DECOR_NEON && active) gfx_fill(c, X, Y + TH - 2, tw, 2, T->c[C_ACCENT] | 0xff000000);
        if (B > 0 && T->decor != DECOR_TAB) gfx_round_frame(c, X, Y, OW, OH, r, bc);
        break;
    }
    }

    struct btn bt[8];
    int nb = button_rects(wn, bt), left_end = X + B + 6, right_start = X + OW - B - 6;
    for (int i = 0; i < nb; i++) {
        paint_button(c, &bt[i], active);
        if (bt[i].x < X + OW / 2 && bt[i].x + bt[i].w + 6 > left_end) left_end = bt[i].x + bt[i].w + 6;
        if (bt[i].x > X + OW / 2 && bt[i].x - 6 < right_start) right_start = bt[i].x - 6;
    }
    int ty = title_y + (TH - 16) / 2;
    int avail = right_start - left_end, tw = gfx_text_width(tfont, wn->title);
    if (T->title_center || T->decor == DECOR_PLATINUM) text_left = X + (OW - tw) / 2;
    else text_left = left_end;
    if (text_left < left_end) text_left = left_end;
    if (T->decor == DECOR_PLATINUM)
        gfx_fill(c, text_left - 6, title_y + 2, (tw < avail ? tw : avail) + 12, TH - 4, ta);
    gfx_text_fit(c, tfont, text_left, ty, avail, wn->title, tf);
}

/* ── apps ───────────────────────────────────────────────────────────── */
static void app_terminal(struct canvas *c, struct win *wn)
{
    int gw, gh, cxp, cyp;
    uint32_t gen;
    console_grid_info(&gw, &gh, &cxp, &cyp, &gen);
    const uint32_t *pal = console_palette();
    int X = client_x(wn), Y = client_y(wn);
    gfx_fill(c, X, Y, wn->w, wn->h, pal[0] | 0xff000000);
    for (int y = 0; y < gh; y++)
        for (int x = 0; x < gw; x++) {
            uint16_t cell = term_cells[y * gw + x];
            int ch = cell & 0xff, fg = (cell >> 8) & 15, bg = (cell >> 12) & 15;
            if (bg) gfx_fill(c, X + 4 + x * 8, Y + 4 + y * 16, 8, 16, pal[bg] | 0xff000000);
            if (ch && ch != ' ') {
                char s[4] = { 0 };
                /* CP437 glyphs the tools use → Unicode for the font */
                static const char *const cp[256] = { [0x07] = "●", [0x09] = "○", [0xC4] = "─", [0xDB] = "█",
                    [0x1E] = "▲", [0xAF] = "❯", [0x03] = "♥", [0x04] = "✦", [0xFA] = "·", [0xFB] = "✓", [0x1A] = "→" };
                const char *str = cp[ch];
                if (!str) { s[0] = (char)(ch < 127 ? ch : '?'); str = s; }
                gfx_text(c, &font_mono, X + 4 + x * 8, Y + 4 + y * 16, str, pal[fg] | 0xff000000);
            }
        }
    if (wn == &wins[focus] && (blink & 1) == 0)
        gfx_fill(c, X + 4 + cxp * 8, Y + 4 + cyp * 16 + 13, 8, 2, pal[7] | 0xff000000);
}

#define CARD_W 214
#define CARD_H 92
static int settings_cols(const struct win *wn) { int n = (wn->w - 16) / (CARD_W + 10); return n < 1 ? 1 : n; }

static void theme_card(struct canvas *c, int x, int y, int i, int selected)
{
    const struct chrono_theme *t = &chrono_themes[i];
    uint32_t fg = T->c[C_WINDOW_FG] | 0xff000000;
    gfx_round(c, x, y, CARD_W, CARD_H, 8, selected ? T->c[C_ACCENT] | 0xff000000 : gfx_shade(T->c[C_WINDOW_BG], 0.06f) | 0xff000000, 0);
    /* a little preview: desktop colours, a window in the theme's title colour, its accent */
    int px = x + 6, py = y + 6, pw = CARD_W - 12, ph = 52;
    gfx_round_vgrad(c, px, py, pw, ph, 6, t->c[C_DESKTOP1] | 0xff000000, t->c[C_DESKTOP2] | 0xff000000, 0);
    gfx_round(c, px + 18, py + 10, pw - 70, ph - 16, t->retro ? 0 : 4, t->c[C_WINDOW_BG] | 0xff000000, 0);
    gfx_round(c, px + 18, py + 10, pw - 70, 10, t->retro ? 0 : 4, t->c[C_TITLE_ACTIVE] | 0xff000000, 1);
    if (t->retro) gfx_hgrad(c, px + 18, py + 10, pw - 70, 10, t->c[C_TITLE_ACTIVE], t->c[C_TITLE_ACTIVE2]);
    gfx_circle(c, px + pw - 30, py + ph / 2.0f, 9, t->c[C_ACCENT] | 0xff000000);
    gfx_fill(c, px, py + ph - 8, pw, 8, t->c[C_PANEL_BG]);
    uint32_t tc = selected ? T->c[C_ACCENT_FG] | 0xff000000 : fg;
    gfx_text_fit(c, &font_bold, x + 8, y + CARD_H - 30, CARD_W - 16, t->name, tc);
    gfx_text(c, &font_ui, x + 8, y + CARD_H - 16, t->retro ? "retro" : "modern", with_alpha(tc, 160));
}

static void app_settings(struct canvas *c, struct win *wn)
{
    int X = client_x(wn), Y = client_y(wn);
    uint32_t fg = T->c[C_WINDOW_FG] | 0xff000000;
    gfx_fill(c, X, Y, wn->w, wn->h, T->c[C_WINDOW_BG] | 0xff000000);
    gfx_text(c, &font_bold, X + 12, Y + 10, "Themes", fg);
    char line[96];
    ksnprintf(line, sizeof line, "%d Chrono themes · click one to apply · scroll for more", CHRONO_THEME_COUNT);
    gfx_text(c, &font_ui, X + 80, Y + 10, line, with_alpha(fg, 150));
    /* Auxo accent: colours the terminal, prompt and Auxo tools */
    int ax = X + 12, ay = Y + wn->h - 34;
    gfx_text(c, &font_bold, ax, ay + 6, "Auxo accent", fg);
    for (int i = 0; i < AUXO_ACCENT_COUNT; i++) {
        float cx = ax + 110 + i * 34.0f, cy = ay + 14.0f;
        if (i == auxo.accent) gfx_ring(c, cx, cy, 13, 2, fg);
        gfx_circle(c, cx, cy, 10, auxo_accents[i].c1 | 0xff000000);
    }
    gfx_clip(c, X, Y + 32, wn->w, wn->h - 76);
    int cols = settings_cols(wn);
    for (int i = 0; i < CHRONO_THEME_COUNT; i++) {
        int row = i / cols - wn->scroll, col = i % cols;
        int x = X + 12 + col * (CARD_W + 10), y = Y + 36 + row * (CARD_H + 10);
        if (y + CARD_H < Y + 32 || y > Y + wn->h) continue;
        theme_card(c, x, y, i, i == cur);
    }
    gfx_noclip(c);
}

static void app_about(struct canvas *c, struct win *wn)
{
    int X = client_x(wn), Y = client_y(wn);
    uint32_t fg = T->c[C_WINDOW_FG] | 0xff000000, dim = with_alpha(fg, 150);
    gfx_fill(c, X, Y, wn->w, wn->h, T->c[C_WINDOW_BG] | 0xff000000);
    /* the Auxo mountain, in the Auxo accent */
    float lx = X + 70, ly = Y + 40, s = 1.1f;
    uint32_t c1 = auxo_c1() | 0xff000000, c2 = auxo_c2() | 0xff000000;
    gfx_line(c, lx - 50 * s, ly + 90 * s, lx, ly, 3, c1);
    gfx_line(c, lx, ly, lx + 50 * s, ly + 90 * s, 3, c2);
    gfx_line(c, lx - 26 * s, ly + 90 * s, lx, ly + 44 * s, 3, gfx_mix(c1, c2, 0.4f));
    gfx_line(c, lx, ly + 44 * s, lx + 26 * s, ly + 90 * s, 3, gfx_mix(c1, c2, 0.6f));
    gfx_circle(c, lx, ly - 12 * s, 6, c1);
    gfx_text(c, &font_bold, X + 150, Y + 30, "AuxoBSD " AUXO_VERSION, fg);
    gfx_text(c, &font_ui, X + 150, Y + 50, "C + Rust + Assembly · x86_64", dim);
    char buf[96], up[32];
    size_t ht, hu;
    rs_heap_stats(&ht, &hu);
    fmt_uptime(up, sizeof up);
    const char *rows[6][2];
    char mem[48], heap[48], thr[16];
    ksnprintf(mem, sizeof mem, "%lu / %lu MiB", (unsigned long)((pmm_total_pages() - pmm_free_pages()) * PAGE_SIZE >> 20),
              (unsigned long)(pmm_total_pages() * PAGE_SIZE >> 20));
    ksnprintf(heap, sizeof heap, "%lu / %lu KiB", (unsigned long)(hu >> 10), (unsigned long)(ht >> 10));
    ksnprintf(thr, sizeof thr, "%lu", (unsigned long)rs_proc_count());
    rows[0][0] = "desktop"; rows[0][1] = "Chrono";
    rows[1][0] = "theme";   rows[1][1] = T->name;
    rows[2][0] = "cpu";     rows[2][1] = cpu_brand();
    rows[3][0] = "memory";  rows[3][1] = mem;
    rows[4][0] = "heap";    rows[4][1] = heap;
    rows[5][0] = "uptime";  rows[5][1] = up;
    for (int i = 0; i < 6; i++) {
        gfx_text(c, &font_ui, X + 150, Y + 84 + i * 20, rows[i][0], with_alpha(auxo_c1(), 255));
        gfx_text_fit(c, &font_ui, X + 230, Y + 84 + i * 20, wn->w - 240, rows[i][1], fg);
    }
    ksnprintf(buf, sizeof buf, "%s threads · %s", thr, AUXO_URL);
    gfx_text(c, &font_ui, X + 150, Y + 84 + 6 * 20, buf, dim);
    gfx_text(c, &font_ui, X + 20, Y + wn->h - 44, "Chrono desktop: github.com/rxvy-dev/chrono", dim);
    gfx_text(c, &font_ui, X + 20, Y + wn->h - 26, "♥ Auxo supports cancer research: terryfox.org", 0xffff5a64);
}

static void paint_window(struct canvas *c, int idx)
{
    struct win *wn = &wins[idx];
    paint_frame(c, wn, idx == focus);
    gfx_clip(c, client_x(wn), client_y(wn), wn->w, wn->h);
    if (wn->app == APP_TERMINAL) app_terminal(c, wn);
    else if (wn->app == APP_SETTINGS) app_settings(c, wn);
    else if (wn->app == APP_ABOUT) app_about(c, wn);
    else {
        struct app_ctx a = { c, client_x(wn), client_y(wn), wn->w, wn->h, &colors, wn == &wins[focus], blink };
        if (wn->app == APP_BROWSER) { browser_paint(&a); wn->title = browser_title(); }
        else if (wn->app == APP_AETHER) { aether_paint(&a); wn->title = aether_title(); }
        else installer_paint(&a);
    }
    gfx_noclip(c);
}

/* ── panel and app menu ─────────────────────────────────────────────── */
struct prect { int x, y, w, h; };
static struct prect panel_rect(void)
{
    int ph = T->panel_height, floating = T->panel_style == PANEL_FLOATING || T->panel_style == PANEL_GLASS;
    int m = floating ? 8 : 0;
    return (struct prect){ m, T->panel_top ? m : H - ph - m, W - 2 * m, ph };
}
static struct prect launcher_rect(void)
{
    struct prect p = panel_rect();
    int bw = T->launcher_label[0] ? gfx_text_width(&font_bold, T->launcher_label) + 36 : p.h;
    return (struct prect){ p.x + 4, p.y + 4, bw, p.h - 8 };
}

static int task_rect(int n, struct prect *r)
{
    struct prect p = panel_rect(), l = launcher_rect();
    *r = (struct prect){ l.x + l.w + 8 + n * 164, p.y + 4, 160, p.h - 8 };
    return r->x + r->w < p.x + p.w - 90;
}

static void paint_logo(struct canvas *c, int x, int y, int s, uint32_t col)
{
    gfx_line(c, x, y + s, x + s / 2.0f, y + 2, 2, col);
    gfx_line(c, x + s / 2.0f, y + 2, x + s, y + s, 2, col);
    gfx_line(c, x + s * 0.28f, y + s, x + s / 2.0f, y + s * 0.55f, 1.6f, col);
    gfx_line(c, x + s / 2.0f, y + s * 0.55f, x + s * 0.72f, y + s, 1.6f, col);
}

static void paint_panel(struct canvas *c)
{
    struct prect p = panel_rect(), l = launcher_rect();
    uint32_t bg = T->c[C_PANEL_BG], fg = T->c[C_PANEL_FG] | 0xff000000;
    int bevel = T->panel_style == PANEL_BEVEL || T->panel_style == PANEL_PLATINUM;
    if (T->panel_style == PANEL_FLOATING || T->panel_style == PANEL_GLASS) {
        gfx_shadow(c, p.x, p.y, p.w, p.h, 12, 12, 70);
        gfx_round(c, p.x, p.y, p.w, p.h, 12, bg, 0);
        gfx_round_frame(c, p.x, p.y, p.w, p.h, 12, T->c[C_PANEL_BORDER]);
    } else {
        gfx_fill(c, p.x, p.y, p.w, p.h, bg);
        if (bevel) gfx_fill(c, p.x, T->panel_top ? p.y + p.h - 1 : p.y, p.w, 1, T->c[C_BEVEL_LIGHT] | 0xff000000);
        else gfx_fill(c, p.x, T->panel_top ? p.y + p.h - 1 : p.y, p.w, 1, T->c[C_PANEL_BORDER]);
        if (T->panel_style == PANEL_GLOSS || T->panel_style == PANEL_MEADOW)
            gfx_vgrad(c, p.x, p.y, p.w, p.h / 2, 0x40ffffff, 0x08ffffff);
    }
    /* launcher */
    uint32_t lbg = menu_open ? T->c[C_ACCENT] | 0xff000000 : 0;
    if (bevel) {
        gfx_fill(c, l.x, l.y, l.w, l.h, T->c[C_BUTTON_BG] | 0xff000000);
        gfx_bevel(c, l.x, l.y, l.w, l.h, T->c[C_BEVEL_LIGHT], T->c[C_BEVEL_DARK], T->c[C_BEVEL_SHADOW], menu_open);
    } else if (lbg) gfx_round(c, l.x, l.y, l.w, l.h, 8, lbg, 0);
    uint32_t lfg = menu_open && !bevel ? T->c[C_ACCENT_FG] | 0xff000000 : fg;
    int ls = l.h - 12 > 18 ? 18 : l.h - 12;
    paint_logo(c, l.x + (T->launcher_label[0] ? 8 : (l.w - ls) / 2), l.y + (l.h - ls) / 2, ls, T->launcher_label[0] ? lfg : auxo_c1() | 0xff000000);
    if (T->launcher_label[0]) gfx_text(c, &font_bold, l.x + 30, l.y + (l.h - 16) / 2, T->launcher_label, lfg);
    /* running apps */
    int n = 0;
    for (int i = 0; i < MAXWIN; i++) {
        if (!wins[i].used) continue;
        struct prect r;
        if (!task_rect(n++, &r)) break;
        int act = i == focus && !wins[i].minimized;
        if (bevel) {
            gfx_fill(c, r.x, r.y, r.w, r.h, act ? gfx_shade(T->c[C_BUTTON_BG], 0.25f) | 0xff000000 : T->c[C_BUTTON_BG] | 0xff000000);
            gfx_bevel(c, r.x, r.y, r.w, r.h, T->c[C_BEVEL_LIGHT], T->c[C_BEVEL_DARK], T->c[C_BEVEL_SHADOW], act);
        } else {
            gfx_round(c, r.x, r.y, r.w, r.h, 7, act ? with_alpha(T->c[C_ACCENT], 70) : 0x14ffffff & (0x00ffffff | (uint32_t)(fg & 0xff000000 ? 0x14 : 0) << 24), 0);
            if (act) gfx_fill(c, r.x + 12, T->panel_top ? r.y : r.y + r.h - 2, r.w - 24, 2, T->c[C_ACCENT] | 0xff000000);
        }
        gfx_text_fit(c, act && bevel ? &font_bold : &font_ui, r.x + 10, r.y + (r.h - 16) / 2, r.w - 20, wins[i].title, fg);
    }
    /* clock */
    struct rtc_time t;
    rtc_read(&t);
    char clk[8];
    ksnprintf(clk, sizeof clk, "%02d:%02d", t.hour, t.minute);
    int cw = gfx_text_width(&font_ui, clk) + 20;
    if (bevel) {
        gfx_bevel(c, p.x + p.w - cw - 6, p.y + 4, cw, p.h - 8, T->c[C_BEVEL_LIGHT], T->c[C_BEVEL_DARK], T->c[C_BEVEL_SHADOW], 1);
    }
    gfx_text(c, &font_ui, p.x + p.w - cw + 4, p.y + (p.h - 16) / 2, clk, fg);
}

enum { MI_TERMINAL, MI_BROWSER, MI_AETHER, MI_SETTINGS, MI_ABOUT, MI_INSTALL, MI_SEP, MI_REBOOT, MI_POWEROFF, MI_COUNT };
static const char *const MENU[] = { "Terminal", "Browser", "Aether", "Settings", "About AuxoBSD", "Install AuxoBSD…", "", "Restart", "Power off" };

static struct prect menu_rect(void)
{
    struct prect p = panel_rect(), l = launcher_rect();
    int h = 44 + (MI_COUNT - 1) * 30 + 10;
    return (struct prect){ l.x, T->panel_top ? p.y + p.h + 4 : p.y - h - 4, 260, h };
}

static int menu_item_at(int x, int y)
{
    struct prect m = menu_rect();
    if (x < m.x || x >= m.x + m.w || y < m.y + 44) return -1;
    int yy = m.y + 44;
    for (int i = 0; i < MI_COUNT; i++) {
        int h = i == MI_SEP ? 10 : 30;
        if (y >= yy && y < yy + h) return i == MI_SEP ? -1 : i;
        yy += h;
    }
    return -1;
}

static void paint_menu(struct canvas *c)
{
    struct prect m = menu_rect();
    uint32_t bg = T->c[C_WINDOW_BG] | 0xff000000, fg = T->c[C_WINDOW_FG] | 0xff000000;
    int retro = T->retro;
    if (retro) {
        gfx_fill(c, m.x, m.y, m.w, m.h, bg);
        gfx_bevel(c, m.x, m.y, m.w, m.h, T->c[C_BEVEL_LIGHT], T->c[C_BEVEL_DARK], T->c[C_BEVEL_SHADOW], 0);
        gfx_vgrad(c, m.x + 3, m.y + 3, m.w - 6, 36, T->c[C_TITLE_ACTIVE], T->c[C_TITLE_ACTIVE2]);
    } else {
        gfx_shadow(c, m.x, m.y, m.w, m.h, 12, 16, 110);
        gfx_round(c, m.x, m.y, m.w, m.h, 12, bg, 0);
        gfx_round_frame(c, m.x, m.y, m.w, m.h, 12, T->c[C_BORDER_ACTIVE]);
    }
    uint32_t hfg = retro ? T->c[C_TITLE_ACTIVE_FG] | 0xff000000 : fg;
    paint_logo(c, m.x + 14, m.y + 10, 20, retro ? hfg : auxo_c1() | 0xff000000);
    gfx_text(c, &font_bold, m.x + 44, m.y + 13, "AuxoBSD", hfg);
    int yy = m.y + 44;
    for (int i = 0; i < MI_COUNT; i++) {
        if (i == MI_SEP) { gfx_fill(c, m.x + 12, yy + 4, m.w - 24, 1, with_alpha(fg, 50)); yy += 10; continue; }
        int hot = i == hover_item;
        if (hot) {
            if (retro) gfx_fill(c, m.x + 4, yy, m.w - 8, 30, T->c[C_SELECTION] | 0xff000000);
            else gfx_round(c, m.x + 6, yy, m.w - 12, 30, 7, T->c[C_ACCENT] | 0xff000000, 0);
        }
        uint32_t col = hot ? (retro ? gfx_text_on(T->c[C_SELECTION]) : T->c[C_ACCENT_FG] | 0xff000000) : fg;
        gfx_text(c, &font_ui, m.x + 18, yy + 7, MENU[i], col);
        yy += 30;
    }
}

/* ── cursor ─────────────────────────────────────────────────────────── */
static const char *const CURSOR[] = {
    "X", "XX", "X.X", "X..X", "X...X", "X....X", "X.....X", "X......X", "X.......X", "X........X",
    "X.........X", "X......XXXXX", "X...X..X", "X..XX..X", "X.X  X..X", "XX   X..X", "X     X..X", "      X..X", "       XX",
};
#define CUR_W 12
#define CUR_H 19
static uint32_t under[CUR_W * CUR_H];
static int cur_drawn_x = -1, cur_drawn_y = -1;

static void cursor_show(void)
{
    /* compose the cursor over the back buffer into a small tile and send just that */
    for (int y = 0; y < CUR_H; y++)
        for (int x = 0; x < CUR_W; x++) {
            int sx = mx + x, sy = my + y;
            uint32_t p = (sx < W && sy < H) ? scr.px[sy * scr.stride + sx] : 0;
            char k = x < (int)strlen(CURSOR[y]) ? CURSOR[y][x] : ' ';
            under[y * CUR_W + x] = k == 'X' ? 0x000000 : k == '.' ? 0xffffff : p;
        }
    /* fb_present reads its source in screen coordinates: hand it a shifted pointer */
    fb_present(under - (my * CUR_W + mx), CUR_W, mx, my, CUR_W < W - mx ? CUR_W : W - mx, CUR_H < H - my ? CUR_H : H - my);
    cur_drawn_x = mx; cur_drawn_y = my;
}

static void cursor_hide(void)
{
    if (cur_drawn_x < 0) return;
    fb_present(scr.px, scr.stride, cur_drawn_x, cur_drawn_y, CUR_W, CUR_H);
    cur_drawn_x = -1;
}

/* ── compositing ────────────────────────────────────────────────────── */
static void compose(void)
{
    memcpy(scr.px, wallc.px, (size_t)W * (size_t)H * 4);
    for (int k = 0; k < nz; k++) {
        int i = zorder[k];
        if (wins[i].used && !wins[i].minimized) paint_window(&scr, i);
    }
    paint_panel(&scr);
    if (menu_open) paint_menu(&scr);
    fb_present(scr.px, scr.stride, 0, 0, W, H);
    cur_drawn_x = -1;
    cursor_show();
}

static void make_colors(void)
{
    uint32_t bg = T->c[C_WINDOW_BG] | 0xff000000, fg = T->c[C_WINDOW_FG] | 0xff000000;
    int lum = (int)(((bg >> 16) & 255) * 3 + ((bg >> 8) & 255) * 6 + (bg & 255)) / 10;
    colors.bg = bg;
    colors.fg = fg;
    colors.dim = gfx_mix(fg, bg, 0.42f) | 0xff000000;
    colors.accent = T->c[C_ACCENT] | 0xff000000;
    colors.accent_fg = T->c[C_ACCENT_FG] | 0xff000000;
    colors.sel = with_alpha(T->c[C_ACCENT], 90);
    colors.border = T->c[C_BORDER_ACTIVE] | 0xff000000;
    colors.field = gfx_shade(bg, lum > 128 ? -0.05f : 0.07f) | 0xff000000;
    colors.button = colors.accent;
    colors.button_fg = colors.accent_fg;
    colors.danger = 0xffe5484d;
    colors.light = lum > 128;
    colors.retro = T->retro;
}

static void set_theme(int i)
{
    cur = i;
    T = &chrono_themes[i];
    make_colors();
    paint_wallpaper(&wallc);
}

void chrono_set_theme_by_id(const char *id) { int i = chrono_theme_find(id); if (i >= 0) set_theme(i); }

/* ── windows ────────────────────────────────────────────────────────── */
static void raise(int i)
{
    int k = 0;
    for (int j = 0; j < nz; j++) if (zorder[j] != i) zorder[k++] = zorder[j];
    zorder[k++] = i;
    nz = k;
    focus = i;
    wins[i].minimized = 0;
}

static void refocus(void)
{
    focus = -1;
    for (int j = nz - 1; j >= 0; j--) if (!wins[zorder[j]].minimized) { focus = zorder[j]; break; }
}

static void minimize(int i) { wins[i].minimized = 1; refocus(); }

static void close_win(int i)
{
    wins[i].used = 0;
    int k = 0;
    for (int j = 0; j < nz; j++) if (zorder[j] != i) zorder[k++] = zorder[j];
    nz = k;
    refocus();
}

static void open_app(int app)
{
    for (int i = 0; i < MAXWIN; i++)
        if (wins[i].used && wins[i].app == app) { raise(i); return; }
    for (int i = 0; i < MAXWIN; i++) {
        if (wins[i].used) continue;
        struct win *w = &wins[i];
        *w = (struct win){ 0 };
        w->used = 1;
        w->app = app;
        w->title = APP_TITLES[app];
        if (app == APP_TERMINAL) { w->w = TERM_COLS * 8 + 8; w->h = TERM_ROWS * 16 + 8; }
        else if (app == APP_SETTINGS) { w->w = 3 * (CARD_W + 10) + 16; w->h = 470; }
        else if (app == APP_ABOUT) { w->w = 470; w->h = 290; }
        else if (app == APP_BROWSER || app == APP_AETHER) { w->w = W - 140 > 1100 ? 1100 : W - 140; w->h = H - 180 > 760 ? 760 : H - 180; }
        else { w->w = 640; w->h = 460; }
        int stagger = 28 * (i % 5);
        w->x = (W - outer_w(w)) / 2 - 60 + stagger;
        w->y = (H - outer_h(w)) / 2 - 40 + stagger;
        if (w->x < 0) w->x = 0;
        if (w->y < 0) w->y = 0;
        raise(i);
        return;
    }
}

static int win_at(int x, int y)
{
    for (int k = nz - 1; k >= 0; k--) {
        struct win *w = &wins[zorder[k]];
        if (!w->used || w->minimized) continue;
        if (x >= w->x && y >= w->y && x < w->x + outer_w(w) && y < w->y + outer_h(w)) return zorder[k];
    }
    return -1;
}

static void maximize(int i)
{
    struct win *w = &wins[i];
    struct prect p = panel_rect();
    if (w->maximized) { w->x = w->sx; w->y = w->sy; w->w = w->sw; w->h = w->sh; w->maximized = 0; return; }
    w->sx = w->x; w->sy = w->y; w->sw = w->w; w->sh = w->h; w->maximized = 1;
    w->x = 0;
    w->y = T->panel_top ? p.y + p.h : 0;
    int avail = H - p.h - (T->panel_style == PANEL_FLOATING || T->panel_style == PANEL_GLASS ? 16 : 0);
    w->w = W - 2 * border();
    w->h = avail - top_h() - border();
    if (w->app == APP_TERMINAL) { w->w = TERM_COLS * 8 + 8; w->h = TERM_ROWS * 16 + 8; w->x = (W - outer_w(w)) / 2; }
}

static void menu_action(int item)
{
    menu_open = 0;
    hover_item = -1;
    switch (item) {
    case MI_TERMINAL: open_app(APP_TERMINAL); break;
    case MI_BROWSER:  open_app(APP_BROWSER); break;
    case MI_AETHER:   open_app(APP_AETHER); break;
    case MI_INSTALL:  open_app(APP_INSTALLER); break;
    case MI_SETTINGS: open_app(APP_SETTINGS); break;
    case MI_ABOUT:    open_app(APP_ABOUT); break;
    case MI_REBOOT:   machine_reboot();
    case MI_POWEROFF: machine_poweroff();
    }
}

/* returns 1 if something changed and the screen needs composing */
static int on_click(int x, int y)
{
    struct prect l = launcher_rect(), p = panel_rect();
    if (menu_open) {
        int it = menu_item_at(x, y);
        struct prect m = menu_rect();
        if (it >= 0) { menu_action(it); return 1; }
        if (x >= m.x && x < m.x + m.w && y >= m.y && y < m.y + m.h) return 0;
        menu_open = 0;
        if (x >= l.x && x < l.x + l.w && y >= l.y && y < l.y + l.h) return 1;
    }
    if (x >= l.x && x < l.x + l.w && y >= l.y && y < l.y + l.h) { menu_open = 1; return 1; }
    if (y >= p.y && y < p.y + p.h && x >= p.x && x < p.x + p.w) {
        int n = 0;
        for (int i = 0; i < MAXWIN; i++) {
            if (!wins[i].used) continue;
            struct prect r;
            if (!task_rect(n++, &r)) break;
            if (x >= r.x && x < r.x + r.w) {
                if (i == focus && !wins[i].minimized) minimize(i);
                else raise(i);
                return 1;
            }
        }
        return 0;
    }
    int i = win_at(x, y);
    if (i < 0) { if (focus >= 0) { focus = -1; return 1; } return 0; }
    raise(i);
    struct win *w = &wins[i];
    struct btn bt[8];
    int nb = button_rects(w, bt);
    for (int b = 0; b < nb; b++)
        if (x >= bt[b].x && x < bt[b].x + bt[b].w && y >= bt[b].y && y < bt[b].y + bt[b].h) {
            if (bt[b].kind == BTN_CLOSE) close_win(i);
            else if (bt[b].kind == BTN_MINIMIZE) minimize(i);
            else if (bt[b].kind == BTN_MAXIMIZE) maximize(i);
            return 1;
        }
    if (y < client_y(w)) { drag = i; drag_dx = x - w->x; drag_dy = y - w->y; return 1; }
    if (w->app != APP_TERMINAL && x >= w->x + outer_w(w) - 14 && y >= w->y + outer_h(w) - 14) {   /* resize grip */
        resize = i; drag_dx = w->x + outer_w(w) - x; drag_dy = w->y + outer_h(w) - y;
        return 1;
    }
    if (w->app == APP_BROWSER) return browser_click(x - client_x(w), y - client_y(w), w->w, w->h) | 1;
    if (w->app == APP_AETHER) return aether_click(x - client_x(w), y - client_y(w), w->w, w->h) | 1;
    if (w->app == APP_INSTALLER) return installer_click(x - client_x(w), y - client_y(w), w->w, w->h) | 1;
    /* inside the client */
    int cx = x - client_x(w), cy = y - client_y(w);
    if (w->app == APP_SETTINGS) {
        if (cy > w->h - 40) {
            for (int a = 0; a < AUXO_ACCENT_COUNT; a++) {
                int ax = 12 + 110 + a * 34;
                if (cx >= ax - 12 && cx < ax + 12) { auxo_set_accent(a); return 1; }
            }
            return 1;
        }
        int cols = settings_cols(w), col = (cx - 12) / (CARD_W + 10), row = (cy - 36) / (CARD_H + 10) + w->scroll;
        if (cx >= 12 && col < cols && cy >= 36 && (cx - 12) % (CARD_W + 10) < CARD_W) {
            int idx = row * cols + col;
            if (idx >= 0 && idx < CHRONO_THEME_COUNT) set_theme(idx);
        }
    }
    return 1;
}

static int on_wheel(int dz)
{
    int i = win_at(mx, my);
    if (i >= 0 && wins[i].app == APP_BROWSER) return browser_wheel(dz);
    if (i >= 0 && wins[i].app == APP_AETHER) return aether_wheel(dz);
    if (i < 0 || wins[i].app != APP_SETTINGS) return 0;
    struct win *w = &wins[i];
    int rows = (CHRONO_THEME_COUNT + settings_cols(w) - 1) / settings_cols(w), vis = (w->h - 76) / (CARD_H + 10);
    w->scroll += dz > 0 ? 1 : -1;
    if (w->scroll > rows - vis) w->scroll = rows - vis;
    if (w->scroll < 0) w->scroll = 0;
    return 1;
}

static int on_key(int k)
{
    if (k == KEY_SUPER) { menu_open = !menu_open; hover_item = -1; return 1; }
    if (menu_open && k == 27) { menu_open = 0; return 1; }
    if (focus >= 0 && wins[focus].app == APP_BROWSER) return browser_key(k);
    if (focus >= 0 && wins[focus].app == APP_AETHER) return aether_key(k);
    if (focus >= 0 && wins[focus].app == APP_INSTALLER) return installer_key(k);
    if (focus >= 0 && wins[focus].app == APP_TERMINAL) {
        static const char *const vt[] = { [KEY_UP - 0x80] = "\033[A", [KEY_DOWN - 0x80] = "\033[B", [KEY_RIGHT - 0x80] = "\033[C",
            [KEY_LEFT - 0x80] = "\033[D", [KEY_HOME - 0x80] = "\033[H", [KEY_END - 0x80] = "\033[F", [KEY_DEL - 0x80] = "\033[3~",
            [KEY_PGUP - 0x80] = "\033[5~", [KEY_PGDN - 0x80] = "\033[6~", [KEY_SHIFT_TAB - 0x80] = "\t" };
        if (k >= 0x80) { const char *s = k - 0x80 < (int)(sizeof vt / sizeof *vt) ? vt[k - 0x80] : 0; while (s && *s) input_push((uint8_t)*s++); }
        else input_push((uint8_t)k);
        return 0;
    }
    return 0;
}

/* ── main loop ──────────────────────────────────────────────────────── */
void chrono_main(uintptr_t arg)
{
    (void)arg;
    W = fb_width();
    H = fb_height();
    size_t pages = ((size_t)W * (size_t)H * 4 + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t a = pmm_alloc_contig(pages), b = pmm_alloc_contig(pages);
    if (!a || !b) { kprintf("chrono: not enough memory for the desktop\n"); return; }
    canvas_init(&scr, (uint32_t *)(uintptr_t)a, W, H, W);
    canvas_init(&wallc, (uint32_t *)(uintptr_t)b, W, H, W);
    set_theme(cur);
    mx = W / 2; my = H / 2;
    running = 1;
    input_set_gui(1);
    open_app(APP_TERMINAL);
    compose();

    struct mouse_event ev;
    for (;;) {
        int dirty = 0, moved = 0;
        if (req_theme >= 0) { set_theme(req_theme); req_theme = -1; dirty = 1; }
        if (req_open >= 0) { open_app(req_open); req_open = -1; dirty = 1; }
        if (req_redraw) { req_redraw = 0; dirty = 1; }
        if (req_edit) { req_edit = 0; open_app(APP_AETHER); aether_open(req_path); dirty = 1; }
        if (req_url) { req_url = 0; chrono_open_url(req_link); dirty = 1; }
        while (mouse_poll(&ev)) {
            int old = buttons;
            mx += ev.dx; my += ev.dy;
            if (mx < 0) mx = 0;
            if (my < 0) my = 0;
            if (mx >= W) mx = W - 1;
            if (my >= H) my = H - 1;
            moved = 1;
            buttons = ev.buttons;
            if (ev.wheel) dirty |= on_wheel(ev.wheel);
            if ((buttons & 1) && !(old & 1)) dirty |= on_click(mx, my);
            if (!(buttons & 1)) drag = resize = -1;
            if (drag >= 0) { wins[drag].x = mx - drag_dx; wins[drag].y = my - drag_dy; wins[drag].maximized = 0; dirty = 1; }
            if (resize >= 0) {
                struct win *rw = &wins[resize];
                int nw = mx + drag_dx - rw->x - 2 * border(), nh = my + drag_dy - rw->y - top_h() - border();
                rw->w = nw < 320 ? 320 : nw; rw->h = nh < 200 ? 200 : nh;
                rw->maximized = 0;
                dirty = 1;
            }
            {
                int hw = win_at(mx, my);
                if (hw >= 0 && wins[hw].app == APP_BROWSER && drag < 0)
                    dirty |= browser_hover(mx - client_x(&wins[hw]), my - client_y(&wins[hw]));
            }
            if (menu_open) { int h = menu_item_at(mx, my); if (h != hover_item) { hover_item = h; dirty = 1; } }
        }
        int k;
        while ((k = input_gui_key()) >= 0) dirty |= on_key(k);
        for (int i = 0; i < MAXWIN; i++) {
            if (!wins[i].used) continue;
            if (wins[i].app == APP_BROWSER) dirty |= browser_tick();
            if (wins[i].app == APP_INSTALLER) dirty |= installer_tick();
        }
        int gw, gh, cxp, cyp;
        uint32_t gen;
        console_grid_info(&gw, &gh, &cxp, &cyp, &gen);
        if (gen != term_gen) { term_gen = gen; dirty = 1; }
        uint32_t tick = (uint32_t)(kernel_ticks() / (AUXO_HZ / 2));
        if (tick != blink) { blink = tick; for (int i = 0; i < MAXWIN; i++) if (wins[i].used && i == focus && wins[i].app != APP_SETTINGS && wins[i].app != APP_ABOUT) dirty = 1; }
        struct rtc_time t;
        if ((blink & 15) == 0) { rtc_read(&t); if ((uint32_t)t.minute != last_minute) { last_minute = (uint32_t)t.minute; dirty = 1; } }
        if (dirty) compose();
        else if (moved) { cursor_hide(); cursor_show(); }
        wait_tick();
    }
}

/* called right after console_init when there's a framebuffer, so the boot log lands in the
 * Terminal's grid (VGA text memory isn't there in graphics mode) */
void chrono_attach_console(void) { console_attach_grid(term_cells, TERM_COLS, TERM_ROWS); }

int chrono_start(void) { return rs_spawn("chrono", chrono_main, 0); }

void chrono_open_url(const char *u)
{
    open_app(APP_BROWSER);
    browser_navigate(u);
}
