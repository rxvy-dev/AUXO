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

#endif
