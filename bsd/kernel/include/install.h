#ifndef AUXO_INSTALL_H
#define AUXO_INSTALL_H
/* disk/install.c: installing to a disk, and /home/root on the installed disk */

struct install_req { int disk; char hostname[32]; int accent; };

extern volatile int install_state;        /* 0 idle, 1 running, 2 done, 3 failed */
extern volatile int install_pct;
extern char install_msg[160];

int  install_start(const struct install_req *r);
int  store_init(void);                    /* installed system: load /home/root, keep saving it */
int  store_save(void);
const char *store_where(void);            /* disk name, or NULL on a live system */

#endif
