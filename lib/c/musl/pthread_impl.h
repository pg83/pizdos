#pragma once

#include "atomic.h"

/*
 * Just enough of musl's thread internals for stdio's getc.h and putc.h to
 * compile. Every FILE has lock == -1, so their locking paths never run.
 */
struct __pthread {
    int tid;
};

static inline struct __pthread* __pthread_self(void) {
    static struct __pthread self;

    return &self;
}

static inline void __wake(volatile void* address, int count, int private) {
    (void)address;
    (void)count;
    (void)private;
}
