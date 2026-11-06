#pragma once

#include <stdint.h>

static inline int a_ctz_32(uint32_t x) {
    return __builtin_ctz(x);
}

static inline int a_ctz_64(uint64_t x) {
    return __builtin_ctzll(x);
}

static inline int a_ctz_l(unsigned long x) {
    return __builtin_ctzl(x);
}

static inline int a_clz_32(uint32_t x) {
    return __builtin_clz(x);
}

static inline int a_clz_64(uint64_t x) {
    return __builtin_clzll(x);
}

static inline int a_cas(volatile int* p, int expected, int desired) {
    int old = *p;

    if (old == expected) {
        *p = desired;
    }

    return old;
}

static inline int a_swap(volatile int* p, int value) {
    int old = *p;

    *p = value;

    return old;
}
