#pragma once

#include <termios.h>
#include <sys/ioctl.h>

__BEGIN_DECLS

int openpty(int*, int*, char*, const struct termios*, const struct winsize*);
pid_t forkpty(int*, char*, const struct termios*, const struct winsize*) __RETURNS_TWICE;

/*
 * Inline only: the import's call is in the caller's frame, which a
 * later return lands in. There is no definition to point to.
 */
pid_t __kernel_forkpty(int*, char*, const struct termios*, size_t, const struct winsize*, size_t) __RETURNS_TWICE __KERNEL(forkpty);

__INLINE pid_t forkpty(int* __a, char* __b, const struct termios* __c, const struct winsize* __d) {
    return __kernel_forkpty(__a, __b, __c, sizeof(struct termios), __d, sizeof(struct winsize));
}

__END_DECLS
