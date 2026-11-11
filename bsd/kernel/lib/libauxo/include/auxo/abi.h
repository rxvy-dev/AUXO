#ifndef AUXO_ABI_H
#define AUXO_ABI_H

#define AUXO_ABI_VERSION 1

#define AUXO_SYS_EXIT        1
#define AUXO_SYS_WRITE       4
#define AUXO_SYS_GETPID      20
#define AUXO_SYS_SCHED_YIELD 331

#define AUXO_STDIN  0
#define AUXO_STDOUT 1
#define AUXO_STDERR 2

#define AUXO_EBADF  9
#define AUXO_EINVAL 22
#define AUXO_ENOSYS 78

#endif

