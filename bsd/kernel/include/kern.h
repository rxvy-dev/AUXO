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
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strchr(const char *s, int c);
size_t strlcpy(char *d, const char *s, size_t n);
int    katoi(const char *s);

/* pmm.c */
void     pmm_init(uint32_t mbi_addr);
uint64_t pmm_alloc_page(void);
uint64_t pmm_alloc_contig(size_t pages);
void     pmm_free_page(uint64_t addr);
size_t   pmm_free_pages(void);
size_t   pmm_total_pages(void);

/* bootinfo.c: what the Multiboot2 loader told us */
void        bootinfo_init(uint32_t mbi_addr);
const char *boot_cmdline(void);       /* "" when none */
const char *boot_loader(void);        /* e.g. "GRUB 2.12", "" when unknown */

/* input.c: COM1 + PS/2 keyboard, interrupt driven, one shared queue */
void input_init(void);
int  input_getc(void);                /* blocks (hlt) until a byte arrives */
int  input_poll(void);                /* -1 when nothing is queued */
void input_serial_irq(void);
void input_kbd_irq(void);

/* power.c */
void machine_reboot(void) __attribute__((noreturn));
void machine_poweroff(void) __attribute__((noreturn));

/* demo.c */
void demo_start(void);

/* shell.c: auxsh, the kernel shell */
void shell_main(uintptr_t arg);
int  shell_readline(const char *prompt, char *buf, size_t n);   /* -1 on Ctrl-C */

#endif
