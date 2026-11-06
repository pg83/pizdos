#pragma once

#include <termios.h>
#include <sys/ioctl.h>

__BEGIN_DECLS

int openpty(int*, int*, char*, const struct termios*, const struct winsize*) __KERNEL(openpty);
pid_t forkpty(int*, char*, const struct termios*, const struct winsize*) __KERNEL(forkpty);

__END_DECLS
