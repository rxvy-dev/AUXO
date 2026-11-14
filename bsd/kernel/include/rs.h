#ifndef AUXO_RS_H
#define AUXO_RS_H
/* FFI surface of the Rust half of the kernel (rust/src/lib.rs). */
#include <stdint.h>
#include <stddef.h>

void    rs_init(uintptr_t heap_base, size_t heap_size);
int     rs_spawn(const char *name, void (*entry)(uintptr_t), uintptr_t arg);
void   *rs_kmalloc(size_t n);
void    rs_kfree(void *p);
int64_t rs_syscall(uint64_t num, uint64_t a0, uint64_t a1, uint64_t a2);
void    rs_tick(void);
void    rs_reap(void);
void    rs_ps(void);
void    rs_heap_stats(size_t *total, size_t *used);
size_t  rs_proc_count(void);
void    rs_get_tls(uint64_t out[3]);
void    rs_set_tls(const uint64_t v[3], int gs);     /* gs < 0: keep %gs */
int     rs_thread_stat(size_t i, int *pid, uint64_t *ticks, char name[16]);   /* state 0..4, -1 past the end */
void    rs_sleep(uint64_t ticks);         /* 0 = until the next timer tick */
int     rs_current_pid(void);
void    rs_set_ucr3(uint64_t cr3);        /* user page tables of the calling thread */
uint64_t rs_kstack_top(void);
void    rs_yield(void);
void    rs_thread_exit(void) __attribute__((noreturn));

#endif
