#ifndef AUXO_SYSCALL_H
#define AUXO_SYSCALL_H

#include <stdint.h>
#include <auxo/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

int64_t auxo_syscall0(uint64_t number);

int64_t auxo_syscall1(
    uint64_t number,
    uint64_t arg0
);

int64_t auxo_syscall2(
    uint64_t number,
    uint64_t arg0,
    uint64_t arg1
);

int64_t auxo_syscall3(
    uint64_t number,
    uint64_t arg0,
    uint64_t arg1,
    uint64_t arg2
);

__attribute__((noreturn))
void auxo_exit(int status);
int64_t auxo_write(int fd, const void *buffer, uint64_t length);
int64_t auxo_getpid(void);
int64_t auxo_sched_yield(void);

#ifdef __cplusplus
}
#endif

#endif
