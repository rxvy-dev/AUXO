#ifndef AUXO_UNISTD_H
#define AUXO_UNISTD_H

#include <stdint.h>

int64_t auxo_write(int fd, const void *buffer, uint64_t length);
int64_t auxo_getpid(void);
int64_t auxo_sched_yield(void);

#endif
