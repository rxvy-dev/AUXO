#include "kern.h"
#include "gfx.h"
#include "fonts.h"
#include "apps.h"
void installer_paint(const struct app_ctx *a) { gfx_fill(a->c, a->x, a->y, a->w, a->h, a->col->bg); }
int installer_click(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; return 0; }
int installer_key(int k) { (void)k; return 0; }
int installer_tick(void) { return 0; }
