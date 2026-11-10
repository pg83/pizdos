#pragma once

#include <__base.h>

__BEGIN_DECLS

#define UIO_MAXIOV 1024

ssize_t readv(int, const struct iovec*, int);
ssize_t writev(int, const struct iovec*, int);
ssize_t preadv(int, const struct iovec*, int, off_t);
ssize_t pwritev(int, const struct iovec*, int, off_t);

__END_DECLS
