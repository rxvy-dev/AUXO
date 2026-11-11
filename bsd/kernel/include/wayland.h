#ifndef AUXO_WAYLAND_H
#define AUXO_WAYLAND_H
/* Chrono's Wayland server (kern/wayland.c), driven by the Chrono thread.
 * "x" handles are xdg surfaces (toplevels and popups). */
#include "gfx.h"

int  wl_server_init(int screen_w, int screen_h);    /* /run/user/0/wayland-0 */
int  wl_server_poll(void);                          /* read clients; 1 = repaint */
void wl_frame_done(void);                           /* after a frame went to the screen */
void wl_paint_window(struct canvas *c, int x, int sx, int sy);   /* geometry origin at sx,sy */
void wl_paint_popups(struct canvas *c);
const char *wl_title(int x);
void wl_geometry(int x, int *gx, int *gy, int *gw, int *gh);

int  wl_popup_at(int sx, int sy, int *bx, int *by);
int  wl_any_popup(void);
void wl_dismiss_popups(void);
void wl_pointer_at(int x, int bx, int by);          /* buffer coordinates; x = -1: nothing */
void wl_button(int button, int pressed);            /* Linux BTN_LEFT 0x110 … */
void wl_axis(int dz);
void wl_focus(int x);
void wl_key(int code, int pressed);                 /* Linux keycode */
void wl_configure_size(int x, int w, int h);
void wl_close(int x);

/* chrono.c, for the server */
int  chrono_wl_new(int x, int w, int h, const char *title);     /* → window index */
void chrono_wl_resized(int win, int w, int h);
void chrono_wl_title(int win, const char *title);
void chrono_wl_close(int win);
void chrono_wl_move(int win);
void chrono_wl_resize(int win, int edges);
void chrono_wl_maximize(int win, int on);
void chrono_wl_minimize(int win);
void chrono_wl_win_pos(int win, int *x, int *y);
void chrono_wl_max_size(int *w, int *h);

#endif
