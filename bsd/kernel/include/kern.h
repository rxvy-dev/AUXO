#ifndef AUXO_KERN_H
#define AUXO_KERN_H
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

#define AUXO_NAME    "AuxoBSD"
#ifndef AUXO_VERSION
#define AUXO_VERSION "0.1.0"          /* normally passed in by the Makefile from bsd/VERSION */
#endif
#define AUXO_URL     "auxolinux.com"
#define AUXO_HZ      100              /* PIT tick rate */
#define PAGE_SIZE    4096UL

/* console.c: VGA text + COM1 serial. ANSI SGR colours (16, 256 and 24-bit) are drawn on
 * VGA by mapping them onto a 16-entry palette whose slots 1-6 hold the accent gradient. */
void console_init(void);
void console_putc(char c);
void console_write(const char *buf, size_t len);
void console_clear(void);
void console_set_accent(uint32_t c1, uint32_t c2);   /* reprograms the VGA DAC: recolours the screen */
void console_serial_color(int on);                    /* strip ANSI escapes from serial when off */
int  console_serial_present(void);                    /* 0 when there is no UART (VirtualBox default) */

/* kprintf.c: %d %i %u %x %X %p %s %c %% with -/0 flags, width, l/z length */
int  kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int  kvprintf(const char *fmt, va_list ap);
int  ksnprintf(char *buf, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
void panic(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));

/* string.c */
void  *memset(void *d, int c, size_t n);
void  *memcpy(void *d, const void *s, size_t n);
void  *memmove(void *d, const void *s, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
void  *memchr(const void *p, int c, size_t n);
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);
size_t strlcpy(char *d, const char *s, size_t n);
int    katoi(const char *s);

/* pmm.c */
void     pmm_init(uint32_t mbi_addr);
int      pmm_ready(void);
uint64_t pmm_alloc_page(void);
uint64_t pmm_alloc_contig(size_t pages);          /* below 4 GiB, like pmm_alloc_page */
uint64_t pmm_alloc_page_any(void);               /* above 4 GiB if there is RAM there (not for DMA) */
uint64_t pmm_alloc_contig_any(size_t pages);
uint64_t pmm_map_high(void);
int      pmm_grow(uint64_t addr, size_t oldpages, size_t newpages);   /* in place, or -1 */                     /* map RAM above 4 GiB; bytes mapped */
void     pmm_free_page(uint64_t addr);
size_t   pmm_free_pages(void);
size_t   pmm_total_pages(void);

/* bootinfo.c: what the Multiboot2 loader told us */
struct fbinfo {
    uint64_t addr;
    uint32_t pitch, width, height;
    uint8_t  bpp, type, rpos, rsize, gpos, gsize, bpos, bsize;
};
const struct fbinfo *boot_framebuffer(void);     /* NULL in text mode */
void        bootinfo_init(uint32_t mbi_addr);
const char *boot_cmdline(void);       /* "" when none */
const char *boot_loader(void);        /* e.g. "GRUB 2.12", "" when unknown */
int         boot_module(const char *name, const uint8_t **data, size_t *len);   /* 0 if GRUB loaded it */

/* input.c: COM1 + PS/2 keyboard, interrupt driven, one shared queue */
void input_init(void);
int  input_getc(void);                /* blocks (hlt) until a byte arrives */
int  input_poll(void);                /* -1 when nothing is queued */
int  input_peek(void);
void input_serial_irq(void);
void input_kbd_irq(void);

/* fb.c: the linear framebuffer (graphics mode) */
int  fb_init(void);                               /* 1 if a usable 15/16/24/32-bit RGB framebuffer is there */
int  fb_width(void);
uint64_t fb_phys(void);                         /* where the framebuffer is (0: none) */
int  fb_height(void);
void fb_present(const uint32_t *src, int stride, int x, int y, int w, int h);   /* XRGB → screen */
void fb_blit(const uint32_t *px, int stride, int x, int y, int w, int h);   /* a small XRGB picture → (x,y) */
void fb_scroll(int rows, uint32_t bg);              /* the text console: everything up, rows of bg */
void fbcon_cell(int x, int y, uint16_t cell, const uint32_t *pal);   /* one 8x16 console cell */
void console_use_fb(void);                          /* the console draws on the framebuffer itself */
void console_fb_takeover(void);                     /* panic: leave the desktop, show the console */

/* mouse.c: PS/2 mouse on IRQ12 (IntelliMouse wheel when available) */
struct mouse_event { int dx, dy, wheel, buttons, abs, ax, ay; };   /* abs: ax, ay are 0..32767 */
int  mouse_init(void);                            /* 0 if there's no PS/2 mouse */
int  mouse_poll(struct mouse_event *ev);          /* 1 when an event was taken */
void mouse_irq(void);
void mouse_byte(uint8_t b);

/* rtc.c: the CMOS real-time clock */
struct rtc_time { int year, month, day, hour, minute, second; };
void rtc_read(struct rtc_time *t);

/* console.c: the grid the Chrono Terminal window draws */
void console_attach_grid(uint16_t *cells, int w, int h);
void console_grid_info(int *w, int *h, int *cur_x, int *cur_y, uint32_t *gen);
const uint32_t *console_palette(void);
void input_push(uint8_t c);                       /* feed the shell, as if typed */
void input_set_gui(int on);                       /* keyboard goes to the desktop instead of the shell */
int  input_gui_key(void);                         /* -1 when none; KEY_* or a byte */
#define KEY_SUPER 0x81
#define KEY_UP    0x82                            /* the desktop gets these instead of VT100 escapes */
#define KEY_DOWN  0x83
#define KEY_LEFT  0x84
#define KEY_RIGHT 0x85
#define KEY_HOME  0x86
#define KEY_END   0x87
#define KEY_DEL   0x88
#define KEY_PGUP  0x89
#define KEY_PGDN  0x8A
#define KEY_SHIFT_TAB 0x8B
int  input_raw_key(void);                         /* desktop: Linux keycode | 0x8000 on release, -1 none */
int  input_shift(void);                           /* shift held (for selections) */
int  input_ctrl(void);

/* power.c */
void machine_reboot(void) __attribute__((noreturn));
void machine_poweroff(void) __attribute__((noreturn));

/* demo.c */
void demo_start(void);

/* shell.c: auxsh, the kernel shell */
void shell_main(uintptr_t arg);
int  shell_readline(const char *prompt, char *buf, size_t n);   /* -1 on Ctrl-C */

#endif
