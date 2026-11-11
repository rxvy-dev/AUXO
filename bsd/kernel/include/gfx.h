#ifndef AUXO_GFX_H
#define AUXO_GFX_H
/* 2D drawing for the Chrono desktop: 32-bit XRGB canvases, colours as 0xAARRGGBB (alpha is
 * blended). gfx.c and the desktop use SSE floating point, so only the Chrono thread may call
 * anything here (the rest of the kernel is integer-only; see Makefile GUI_CFLAGS). */
#include <stdint.h>
#include "fonts.h"

struct canvas {
    uint32_t *px;
    int w, h, stride;             /* stride in pixels */
    int cx0, cy0, cx1, cy1;       /* clip rectangle, exclusive end */
};

static inline uint32_t argb(int a, int r, int g, int b) { return (uint32_t)a << 24 | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b; }
static inline int ca(uint32_t c) { return (int)(c >> 24); }
static inline uint32_t with_alpha(uint32_t c, int a) { return (c & 0xffffff) | (uint32_t)(a < 0 ? 0 : a > 255 ? 255 : a) << 24; }

void     canvas_init(struct canvas *c, uint32_t *px, int w, int h, int stride);
void     gfx_clip(struct canvas *c, int x, int y, int w, int h);   /* intersects with the current clip */
void     gfx_noclip(struct canvas *c);
uint32_t gfx_mix(uint32_t a, uint32_t b, float t);                 /* per channel, alpha too */
uint32_t gfx_shade(uint32_t c, float k);                           /* k<0 darker, k>0 lighter */
uint32_t gfx_text_on(uint32_t bg);                                 /* black or white, readable on bg */

void gfx_pixel(struct canvas *c, int x, int y, uint32_t col);
void gfx_fill(struct canvas *c, int x, int y, int w, int h, uint32_t col);
void gfx_frame(struct canvas *c, int x, int y, int w, int h, int t, uint32_t col);
void gfx_vgrad(struct canvas *c, int x, int y, int w, int h, uint32_t top, uint32_t bottom);
void gfx_hgrad(struct canvas *c, int x, int y, int w, int h, uint32_t left, uint32_t right);
void gfx_round(struct canvas *c, int x, int y, int w, int h, int r, uint32_t col, int top_only);
void gfx_round_vgrad(struct canvas *c, int x, int y, int w, int h, int r, uint32_t top, uint32_t bottom, int top_only);
void gfx_round_frame(struct canvas *c, int x, int y, int w, int h, int r, uint32_t col);
void gfx_circle(struct canvas *c, float cx, float cy, float r, uint32_t col);
void gfx_ring(struct canvas *c, float cx, float cy, float r, float width, uint32_t col);
void gfx_radial(struct canvas *c, float cx, float cy, float r, uint32_t col, int screen);  /* alpha → 0 at r */
void gfx_line(struct canvas *c, float x0, float y0, float x1, float y1, float width, uint32_t col);
void gfx_column(struct canvas *c, int x, float ytop, int ybottom, uint32_t col);         /* fill below a curve */
void gfx_bevel(struct canvas *c, int x, int y, int w, int h, uint32_t light, uint32_t dark, uint32_t shadow, int sunken);
void gfx_shadow(struct canvas *c, int x, int y, int w, int h, int r, int size, int alpha);
void gfx_blit(struct canvas *dst, int x, int y, const struct canvas *src);

int  gfx_text(struct canvas *c, const struct font *f, int x, int y, const char *s, uint32_t col);  /* y = top */
int  gfx_text_width(const struct font *f, const char *s);
void gfx_text_fit(struct canvas *c, const struct font *f, int x, int y, int maxw, const char *s, uint32_t col);
const struct glyph *gfx_glyph(const struct font *f, uint32_t cp);

float gfx_sin(float x);
float gfx_cos(float x);
float gfx_sqrt(float x);
float gfx_floor(float x);
#define GFX_PI 3.14159265f

#endif
