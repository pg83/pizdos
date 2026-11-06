#pragma once

#include <__base.h>

__BEGIN_DECLS

struct utimbuf {
    time_t actime;
    time_t modtime;
};

int utime(const char*, const struct utimbuf*) __KERNEL(utime);

__END_DECLS
