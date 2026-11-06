#pragma once

#include <__base.h>

#define offsetof(type, member) __builtin_offsetof(type, member)

typedef struct {
    long long __ll __attribute__((__aligned__(__alignof__(long long))));
    long double __ld __attribute__((__aligned__(__alignof__(long double))));
} max_align_t;
