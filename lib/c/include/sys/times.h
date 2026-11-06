#pragma once

#include <__base.h>

__BEGIN_DECLS

struct tms {
    clock_t tms_utime;
    clock_t tms_stime;
    clock_t tms_cutime;
    clock_t tms_cstime;
};

clock_t times(struct tms*) __KERNEL(times);

__END_DECLS
