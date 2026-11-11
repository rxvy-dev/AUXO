#ifndef AUXO_CHRONO_H
#define AUXO_CHRONO_H
/* The Chrono desktop (kern/chrono.c). Safe to call from any thread: these only set requests
 * the Chrono thread picks up. */
#include <stdint.h>

#define CHRONO_THEMES 43        /* checked against chrono_themes.h in kern/chrono.c */

enum { APP_TERMINAL, APP_SETTINGS, APP_ABOUT, APP_BROWSER, APP_AETHER, APP_INSTALLER, APP_COUNT };

void chrono_attach_console(void);     /* console output → the Terminal window's grid */
int  chrono_start(void);              /* start the desktop thread */
int  chrono_running(void);
int  chrono_current_theme(void);
int  chrono_theme_find(const char *id);     /* -1 if unknown */
const char *chrono_theme_id(int index);    /* NULL past the end */
const char *chrono_theme_name(int index);
int  chrono_theme_retro(int index);
void chrono_request_theme(int index);
void chrono_request_open(int app);
void chrono_request_redraw(void);
void chrono_request_edit(const char *path);   /* open a file in Aether */
void chrono_request_url(const char *url);     /* open a page in the Browser */
void chrono_main(uintptr_t arg);

#endif
