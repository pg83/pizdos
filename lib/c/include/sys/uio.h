#pragma once

#include <__base.h>

__BEGIN_DECLS

#define UIO_MAXIOV 1024

ssize_t readv(int, const struct iovec*, int) __KERNEL(readv);
ssize_t writev(int, const struct iovec*, int) __KERNEL(writev);
ssize_t preadv(int, const struct iovec*, int, off_t) __KERNEL(preadv);
ssize_t pwritev(int, const struct iovec*, int, off_t) __KERNEL(pwritev);

__END_DECLS
