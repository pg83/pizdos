#pragma once

#include <features.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

struct __locale_struct {
    const void* cat[6];
};

struct __libc {
    char threaded;
    char secure;
    volatile signed char need_locks;
};

extern hidden struct __libc __libc;

#define libc __libc
