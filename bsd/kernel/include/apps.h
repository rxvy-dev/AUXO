#ifndef AUXO_APPS_H
#define AUXO_APPS_H
/* The Chrono apps that live in their own files (Browser, Aether, Installer). Chrono gives each
 * one its client rectangle and the theme's colours; coordinates passed in are client-relative.
 * All of this runs on the Chrono thread (SSE). Each *_click/_key/_wheel/_tick returns 1 when
 * the window needs repainting. */
#include <stdint.h>
#include "gfx.h"

struct app_colors {
    uint32_t bg, fg, dim, accent, accent_fg, sel, border, field, button, button_fg, danger;
    int light, retro;
};

struct app_ctx {
    struct canvas *c;
    int x, y, w, h;              /* client rectangle on the canvas */
    const struct app_colors *col;
    int focused;
    uint32_t blink;              /* caret phase */
};

/* Browser (kern/browser.c) */
void        browser_paint(const struct app_ctx *a);
int         browser_click(int x, int y, int w, int h);
int         browser_key(int k);
int         browser_wheel(int dz);
int         browser_hover(int x, int y);
int         browser_tick(void);
void        browser_navigate(const char *url);
const char *browser_title(void);

/* Aether (kern/aether.c) */
void        aether_paint(const struct app_ctx *a);
int         aether_click(int x, int y, int w, int h);
int         aether_key(int k);
int         aether_wheel(int dz);
const char *aether_title(void);
void        aether_open(const char *path);

/* Installer (kern/installer.c) */
void        installer_paint(const struct app_ctx *a);
int         installer_click(int x, int y, int w, int h);
int         installer_key(int k);
int         installer_tick(void);

/* Files (kern/files.c) */
void        files_paint(const struct app_ctx *a);
int         files_click(int x, int y, int w, int h);
int         files_key(int k);
int         files_wheel(int dz);
const char *files_title(void);
void        files_open(const char *path);

/* chrono.c, for the apps */
void chrono_open_url(const char *url);       /* raise the Browser and load url */
void chrono_set_theme_by_id(const char *id);

#endif
