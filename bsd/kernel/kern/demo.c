/* Boot-time demo: kernel threads exercising the syscall gate and scheduler. */
#include "kern.h"
#include "arch.h"
#include "rs.h"

#define SYS_exit        1
#define SYS_write       4
#define SYS_getpid      20
#define SYS_sched_yield 331

static inline long sys3(long n, long a, long b, long c)
{
    long r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "memory");
    return r;
}

static void spin(void) { for (volatile int i = 0; i < 2000000; i++) ; }

static void worker(uintptr_t id)
{
    long pid = sys3(SYS_getpid, 0, 0, 0);
    static const char hello[] = "  write(2) from a kernel thread via int 0x80\n";
    sys3(SYS_write, 1, (long)hello, sizeof hello - 1);

    for (int i = 0; i < 3; i++) {
        kprintf("  [worker %lu] pid=%ld iteration %d (tick %lu)\n", id, pid, i, kernel_ticks());
        spin();
        sys3(SYS_sched_yield, 0, 0, 0);
    }
    if (id == 2) sys3(SYS_exit, 7, 0, 0);       /* explicit exit(7) */
    /* returning falls into thread_trampoline -> rs_thread_exit */
}

void demo_start(void)
{
    rs_spawn("worker1", worker, 1);
    rs_spawn("worker2", worker, 2);
    rs_spawn("worker3", worker, 3);
}
