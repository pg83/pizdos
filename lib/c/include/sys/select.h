#pragma once

#include <__base.h>

__BEGIN_DECLS

#define FD_SETSIZE 1024

typedef unsigned long fd_mask;

typedef struct {
    fd_mask fds_bits[FD_SETSIZE / (8 * sizeof(fd_mask))];
} fd_set;

#define NFDBITS (8 * (int)sizeof(fd_mask))
#define FD_ZERO(s) __builtin_memset((s), 0, sizeof(fd_set))
#define FD_SET(d, s) ((s)->fds_bits[(d) / NFDBITS] |= (1UL << ((d) % NFDBITS)))
#define FD_CLR(d, s) ((s)->fds_bits[(d) / NFDBITS] &= ~(1UL << ((d) % NFDBITS)))
#define FD_ISSET(d, s) (!!((s)->fds_bits[(d) / NFDBITS] & (1UL << ((d) % NFDBITS))))

int select(int, fd_set* __restrict, fd_set* __restrict, fd_set* __restrict, struct timeval* __restrict);
int pselect(int, fd_set* __restrict, fd_set* __restrict, fd_set* __restrict, const struct timespec* __restrict, const sigset_t* __restrict);

__END_DECLS
