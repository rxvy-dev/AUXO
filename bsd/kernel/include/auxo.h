#ifndef AUXO_AUXO_H
#define AUXO_AUXO_H
/* Auxo settings and the Auxo tools (auxo-tweak, auxo-fetch, auxo-motd) on AuxoBSD.
 * The command line and output follow the Auxo Linux tools in packages/auxo-tools. */
#include <stdint.h>
#include "auxo_accents.h"

struct auxo_settings {
    int  accent;            /* index into auxo_accents[] */
    char hostname[32];
    int  motd;              /* show auxo-motd when the shell starts */
    int  text;              /* `text` on the boot line: a plain text console, no desktop */
    int  color;             /* ANSI colour on the serial console */
};
extern struct auxo_settings auxo;

/* auxo.c */
void        auxo_conf_init(const char *cmdline);   /* defaults + accent=, hostname=, motd=, nocolor */
int         auxo_accent_find(const char *name);    /* -1 if unknown */
void        auxo_set_accent(int idx);
const char *auxo_accent_name(void);
uint32_t    auxo_c1(void);
uint32_t    auxo_c2(void);
uint32_t    rgb_lerp(uint32_t a, uint32_t b, int num, int den);
void        c_rgb(uint32_t rgb);                   /* \e[38;2;R;G;Bm */
void        c_rgb_bold(uint32_t rgb);
void        c_dim(void);
void        c_bold(void);
void        c_reset(void);
void        fmt_uptime(char *buf, size_t n);       /* "1d 2h 3m", like auxo-fetch on Linux */
const char *cpu_brand(void);
int         rand_range(int n);                     /* 0..n-1, TSC-seeded */

/* commands (argc/argv, return 0 on success) */
int cmd_auxo_tweak(int argc, char **argv);
int cmd_auxo_fetch(int argc, char **argv);
int cmd_auxo_motd(int argc, char **argv);

#endif
