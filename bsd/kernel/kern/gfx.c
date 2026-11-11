/* 2D drawing for the Chrono desktop. Built with SSE (see GUI_CFLAGS): only the Chrono thread
 * runs this code, so no other thread's floating-point state can be disturbed. */
#define AUXO_FONTS_IMPL
#include "kern.h"
#include "gfx.h"

/* ── maths ──────────────────────────────────────────────────────────── */
float gfx_floor(float x)
{
    int i = (int)x;
    return (float)(x < (float)i ? i - 1 : i);
}

float gfx_sqrt(float x) { return x <= 0 ? 0 : __builtin_sqrtf(x); }

float gfx_sin(float x)
{
    /* reduce to [-pi, pi], then a 7th-order polynomial on [-pi/2, pi/2] */
    const float twopi = 2 * GFX_PI;
    x -= twopi * gfx_floor((x + GFX_PI) / twopi);
    if (x > GFX_PI / 2) x = GFX_PI - x;
    else if (x < -GFX_PI / 2) x = -GFX_PI - x;
    float x2 = x * x;
    return x * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42)));
}

float gfx_cos(float x) { return gfx_sin(x + GFX_PI / 2); }

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* ── colours ────────────────────────────────────────────────────────── */
uint32_t gfx_mix(uint32_t a, uint32_t b, float t)
{
    if (t <= 0) return a;
    if (t >= 1) return b;
    uint32_t out = 0;
    for (int sh = 0; sh <= 24; sh += 8) {
        float x = (float)((a >> sh) & 255), y = (float)((b >> sh) & 255);
        out |= (uint32_t)clampi((int)(x + (y - x) * t + 0.5f), 0, 255) << sh;
    }
    return out;
}

uint32_t gfx_shade(uint32_t c, float k)
{
    return k < 0 ? gfx_mix(c, c & 0xff000000, -k) : gfx_mix(c, c | 0x00ffffff, k);
}

uint32_t gfx_text_on(uint32_t bg)
{
    int r = (bg >> 16) & 255, g = (bg >> 8) & 255, b = bg & 255;
    return (r * 299 + g * 587 + b * 114) / 1000 > 150 ? 0xff000000 : 0xffffffff;
}

/* ── canvas ─────────────────────────────────────────────────────────── */
void canvas_init(struct canvas *c, uint32_t *px, int w, int h, int stride)
{
    c->px = px; c->w = w; c->h = h; c->stride = stride;
    gfx_noclip(c);
}

void gfx_noclip(struct canvas *c) { c->cx0 = 0; c->cy0 = 0; c->cx1 = c->w; c->cy1 = c->h; }

void gfx_clip(struct canvas *c, int x, int y, int w, int h)
{
    if (x > c->cx0) c->cx0 = x;
    if (y > c->cy0) c->cy0 = y;
    if (x + w < c->cx1) c->cx1 = x + w;
    if (y + h < c->cy1) c->cy1 = y + h;
    if (c->cx1 < c->cx0) c->cx1 = c->cx0;
    if (c->cy1 < c->cy0) c->cy1 = c->cy0;
}

static inline void blend(uint32_t *p, uint32_t col, int a)
{
    if (a <= 0) return;
    if (a >= 255) { *p = col & 0xffffff; return; }
    uint32_t d = *p;
    uint32_t rb = ((col & 0xff00ff) * (uint32_t)a + (d & 0xff00ff) * (uint32_t)(255 - a)) >> 8;
    uint32_t g = ((col & 0x00ff00) * (uint32_t)a + (d & 0x00ff00) * (uint32_t)(255 - a)) >> 8;
    *p = (rb & 0xff00ff) | (g & 0x00ff00);
}

static inline void plot(struct canvas *c, int x, int y, uint32_t col, int a)
{
    if (x < c->cx0 || y < c->cy0 || x >= c->cx1 || y >= c->cy1) return;
    blend(&c->px[y * c->stride + x], col, a);
}

void gfx_pixel(struct canvas *c, int x, int y, uint32_t col) { plot(c, x, y, col, ca(col)); }

void gfx_fill(struct canvas *c, int x, int y, int w, int h, uint32_t col)
{
    int x0 = clampi(x, c->cx0, c->cx1), x1 = clampi(x + w, c->cx0, c->cx1);
    int y0 = clampi(y, c->cy0, c->cy1), y1 = clampi(y + h, c->cy0, c->cy1);
    int a = ca(col);
    if (a == 0) return;
    for (int yy = y0; yy < y1; yy++) {
        uint32_t *row = &c->px[yy * c->stride];
        if (a == 255) for (int xx = x0; xx < x1; xx++) row[xx] = col & 0xffffff;
        else for (int xx = x0; xx < x1; xx++) blend(&row[xx], col, a);
    }
}

void gfx_frame(struct canvas *c, int x, int y, int w, int h, int t, uint32_t col)
{
    gfx_fill(c, x, y, w, t, col);
    gfx_fill(c, x, y + h - t, w, t, col);
    gfx_fill(c, x, y + t, t, h - 2 * t, col);
    gfx_fill(c, x + w - t, y + t, t, h - 2 * t, col);
}

void gfx_vgrad(struct canvas *c, int x, int y, int w, int h, uint32_t top, uint32_t bottom)
{
    for (int i = 0; i < h; i++) gfx_fill(c, x, y + i, w, 1, gfx_mix(top, bottom, h > 1 ? (float)i / (float)(h - 1) : 0));
}

void gfx_hgrad(struct canvas *c, int x, int y, int w, int h, uint32_t left, uint32_t right)
{
    for (int i = 0; i < w; i++) gfx_fill(c, x + i, y, 1, h, gfx_mix(left, right, w > 1 ? (float)i / (float)(w - 1) : 0));
}

/* coverage of pixel (px,py) inside a rounded rectangle, antialiased at the corners */
static float round_cover(int px, int py, int x, int y, int w, int h, float r, int top_only)
{
    float fx = (float)px + 0.5f, fy = (float)py + 0.5f, cx, cy;
    if (fx < x + r) cx = x + r; else if (fx > x + w - r) cx = x + w - r; else return 1;
    if (fy < y + r) cy = y + r; else if (fy > y + h - r && !top_only) cy = y + h - r; else return 1;
    float d = gfx_sqrt((fx - cx) * (fx - cx) + (fy - cy) * (fy - cy));
    float cov = r - d + 0.5f;
    return cov < 0 ? 0 : cov > 1 ? 1 : cov;
}

void gfx_round_vgrad(struct canvas *c, int x, int y, int w, int h, int r, uint32_t top, uint32_t bottom, int top_only)
{
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    for (int yy = y; yy < y + h; yy++) {
        if (yy < c->cy0 || yy >= c->cy1) continue;
        uint32_t col = top == bottom ? top : gfx_mix(top, bottom, h > 1 ? (float)(yy - y) / (float)(h - 1) : 0);
        int a = ca(col);
        int corner = (yy < y + r) || (!top_only && yy >= y + h - r);
        if (!corner) { gfx_fill(c, x, yy, w, 1, col); continue; }
        for (int xx = x; xx < x + w; xx++) {
            float cov = round_cover(xx, yy, x, y, w, h, (float)r, top_only);
            if (cov > 0) plot(c, xx, yy, col, (int)(a * cov));
        }
    }
}

void gfx_round(struct canvas *c, int x, int y, int w, int h, int r, uint32_t col, int top_only)
{
    gfx_round_vgrad(c, x, y, w, h, r, col, col, top_only);
}

void gfx_round_frame(struct canvas *c, int x, int y, int w, int h, int r, uint32_t col)
{
    if (r <= 0) { gfx_frame(c, x, y, w, h, 1, col); return; }
    int a = ca(col);
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++) {
            if (yy > y + r && yy < y + h - r - 1 && xx > x && xx < x + w - 1) { xx = x + w - 2; continue; }
            float outer = round_cover(xx, yy, x, y, w, h, (float)r, 0);
            float inner = round_cover(xx, yy, x + 1, y + 1, w - 2, h - 2, (float)(r - 1), 0);
            if (xx == x || xx == x + w - 1 || yy == y || yy == y + h - 1) inner = 0;
            float cov = outer - inner;
            if (cov > 0) plot(c, xx, yy, col, (int)(a * cov));
        }
}

void gfx_circle(struct canvas *c, float cx, float cy, float r, uint32_t col)
{
    int a = ca(col);
    for (int y = (int)(cy - r - 1); y <= (int)(cy + r + 1); y++)
        for (int x = (int)(cx - r - 1); x <= (int)(cx + r + 1); x++) {
            float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy;
            float cov = r - gfx_sqrt(dx * dx + dy * dy) + 0.5f;
            if (cov > 0) plot(c, x, y, col, (int)(a * (cov > 1 ? 1 : cov)));
        }
}

void gfx_ring(struct canvas *c, float cx, float cy, float r, float width, uint32_t col)
{
    int a = ca(col);
    float ro = r + width / 2, ri = r - width / 2;
    int y0 = (int)(cy - ro - 1), y1 = (int)(cy + ro + 1);
    if (y0 < c->cy0) y0 = c->cy0;
    if (y1 >= c->cy1) y1 = c->cy1 - 1;
    for (int y = y0; y <= y1; y++) {
        float dy = (float)y + 0.5f - cy;
        float hw = ro * ro - dy * dy;
        if (hw < 0) continue;
        float xs = gfx_sqrt(hw);
        int x0 = (int)(cx - xs - 1), x1 = (int)(cx + xs + 1);
        if (x0 < c->cx0) x0 = c->cx0;
        if (x1 >= c->cx1) x1 = c->cx1 - 1;
        float inner2 = ri > 0 ? ri * ri - dy * dy : -1;
        float xi = inner2 > 0 ? gfx_sqrt(inner2) : 0;
        for (int x = x0; x <= x1; x++) {
            float dx = (float)x + 0.5f - cx;
            if (inner2 > 0 && dx > -xi + 1 && dx < xi - 1) { x = (int)(cx + xi - 1); continue; }
            float d = gfx_sqrt(dx * dx + dy * dy);
            float cov = (ro - d + 0.5f);
            float cin = (d - ri + 0.5f);
            if (cov > 1) cov = 1;
            if (cin > 1) cin = 1;
            float k = cov < cin ? cov : cin;
            if (k > 0) plot(c, x, y, col, (int)(a * k));
        }
    }
}

void gfx_radial(struct canvas *c, float cx, float cy, float r, uint32_t col, int screen)
{
    int a = ca(col);
    int y0 = clampi((int)(cy - r), c->cy0, c->cy1), y1 = clampi((int)(cy + r + 1), c->cy0, c->cy1);
    int x0 = clampi((int)(cx - r), c->cx0, c->cx1), x1 = clampi((int)(cx + r + 1), c->cx0, c->cx1);
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            float dx = (float)x - cx, dy = (float)y - cy;
            float t = gfx_sqrt(dx * dx + dy * dy) / r;
            if (t >= 1) continue;
            int k = (int)(a * (1 - t));
            uint32_t *p = &c->px[y * c->stride + x];
            if (!screen) { blend(p, col, k); continue; }
            uint32_t d = *p, out = 0;               /* screen: 1 - (1-a)(1-b), weighted by k */
            for (int sh = 0; sh <= 16; sh += 8) {
                int s = (int)((col >> sh) & 255), b = (int)((d >> sh) & 255);
                int scr = 255 - (255 - s) * (255 - b) / 255;
                out |= (uint32_t)(b + (scr - b) * k / 255) << sh;
            }
            *p = out;
        }
}

void gfx_line(struct canvas *c, float x0, float y0, float x1, float y1, float width, uint32_t col)
{
    /* walk the major axis; across it, an antialiased span as thick as the line */
    float dx = x1 - x0, dy = y1 - y0, adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    float len = gfx_sqrt(dx * dx + dy * dy);
    int a = ca(col);
    if (len < 0.01f) { gfx_circle(c, x0, y0, width / 2, col); return; }
    int steep = ady > adx;
    float major0 = steep ? y0 : x0, major1 = steep ? y1 : x1;
    float minor0 = steep ? x0 : y0, slope = steep ? dx / dy : dy / dx;
    float half = width / 2 * len / (steep ? ady : adx);           /* thickness measured across the minor axis */
    if (major1 < major0) { float t = major0; major0 = major1; major1 = t; minor0 = steep ? x1 : y1; }
    for (int m = (int)gfx_floor(major0); m <= (int)gfx_floor(major1); m++) {
        float centre = minor0 + ((float)m + 0.5f - major0) * slope;
        for (int n = (int)gfx_floor(centre - half - 1); n <= (int)gfx_floor(centre + half + 1); n++) {
            float d = (float)n + 0.5f - centre;
            float cov = half - (d < 0 ? -d : d) + 0.5f;
            if (cov <= 0) continue;
            if (cov > 1) cov = 1;
            if (steep) plot(c, n, m, col, (int)(a * cov));
            else plot(c, m, n, col, (int)(a * cov));
        }
    }
}

void gfx_column(struct canvas *c, int x, float ytop, int ybottom, uint32_t col)
{
    if (x < c->cx0 || x >= c->cx1) return;
    int yt = (int)gfx_floor(ytop);
    float frac = ytop - (float)yt;
    int a = ca(col);
    if (yt >= c->cy0 && yt < c->cy1) blend(&c->px[yt * c->stride + x], col, (int)(a * (1 - frac)));
    for (int y = clampi(yt + 1, c->cy0, c->cy1); y < clampi(ybottom, c->cy0, c->cy1); y++)
        blend(&c->px[y * c->stride + x], col, a);
}

void gfx_bevel(struct canvas *c, int x, int y, int w, int h, uint32_t light, uint32_t dark, uint32_t shadow, int sunken)
{
    uint32_t tl = sunken ? dark : light, br = sunken ? light : shadow, br2 = sunken ? light : dark;
    gfx_fill(c, x, y, w - 1, 1, tl);
    gfx_fill(c, x, y, 1, h - 1, tl);
    gfx_fill(c, x, y + h - 1, w, 1, br);
    gfx_fill(c, x + w - 1, y, 1, h, br);
    gfx_fill(c, x + 1, y + h - 2, w - 2, 1, br2);
    gfx_fill(c, x + w - 2, y + 1, 1, h - 2, br2);
}

void gfx_shadow(struct canvas *c, int x, int y, int w, int h, int r, int size, int alpha)
{
    /* a soft drop shadow: stacked translucent rounded rectangles, offset down a little */
    for (int i = size; i > 0; i -= 2) {
        int a = alpha * (size - i + 2) / (size * size / 2 + 1);
        if (a <= 0) continue;
        gfx_round(c, x - i, y - i + size / 3, w + 2 * i, h + 2 * i, r + i, (uint32_t)(a > 255 ? 255 : a) << 24, 0);
    }
}

void gfx_blit(struct canvas *dst, int x, int y, const struct canvas *src)
{
    for (int yy = 0; yy < src->h; yy++) {
        int dy = y + yy;
        if (dy < dst->cy0 || dy >= dst->cy1) continue;
        for (int xx = 0; xx < src->w; xx++) {
            int dx = x + xx;
            if (dx < dst->cx0 || dx >= dst->cx1) continue;
            dst->px[dy * dst->stride + dx] = src->px[yy * src->stride + xx];
        }
    }
}

/* ── text ───────────────────────────────────────────────────────────── */
static uint32_t utf8_next(const char **s)
{
    const uint8_t *p = (const uint8_t *)*s;
    uint32_t c = *p++;
    if (c >= 0xf0 && p[0] && p[1] && p[2]) { c = (c & 7) << 18 | (p[0] & 63u) << 12 | (p[1] & 63u) << 6 | (p[2] & 63u); p += 3; }
    else if (c >= 0xe0 && p[0] && p[1]) { c = (c & 15) << 12 | (p[0] & 63u) << 6 | (p[1] & 63u); p += 2; }
    else if (c >= 0xc0 && p[0]) { c = (c & 31) << 6 | (p[0] & 63u); p += 1; }
    *s = (const char *)p;
    return c;
}

const struct glyph *gfx_glyph(const struct font *f, uint32_t cp)
{
    if (cp >= 32 && cp < 127) return &f->glyphs[cp - 32];        /* ASCII comes first, in order */
    for (int i = 95; i < f->count; i++)
        if (f->glyphs[i].cp == cp) return &f->glyphs[i];
    return &f->glyphs['?' - 32];
}

int gfx_text(struct canvas *c, const struct font *f, int x, int y, const char *s, uint32_t col)
{
    int x0 = x, a = ca(col);
    while (*s) {
        const struct glyph *g = gfx_glyph(f, utf8_next(&s));
        const uint8_t *bits = f->bits + g->offset;
        for (int yy = 0; yy < f->height; yy++) {
            int py = y + yy;
            if (py < c->cy0 || py >= c->cy1) continue;
            for (int xx = 0; xx < g->width; xx++) {
                int k = bits[yy * g->width + xx];
                if (k) plot(c, x + xx, py, col, k * a / 255);
            }
        }
        x += g->advance;
    }
    return x - x0;
}

int gfx_text_width(const struct font *f, const char *s)
{
    int w = 0;
    while (*s) w += gfx_glyph(f, utf8_next(&s))->advance;
    return w;
}

void gfx_text_fit(struct canvas *c, const struct font *f, int x, int y, int maxw, const char *s, uint32_t col)
{
    if (gfx_text_width(f, s) <= maxw) { gfx_text(c, f, x, y, s, col); return; }
    char buf[160];
    int n = 0, w = gfx_text_width(f, "…");
    const char *p = s;
    while (*p && n < (int)sizeof buf - 4) {
        const char *q = p;
        int gw = gfx_glyph(f, utf8_next(&q))->advance;
        if (w + gw > maxw) break;
        while (p < q) buf[n++] = *p++;
        w += gw;
    }
    buf[n] = 0;
    strlcpy(buf + n, "…", sizeof buf - (size_t)n);
    gfx_text(c, f, x, y, buf, col);
}
