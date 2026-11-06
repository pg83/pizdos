#pragma once

#include <__base.h>

__BEGIN_DECLS

#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_NB 4
#define LOCK_UN 8

int flock(int, int) __KERNEL(flock);

__END_DECLS
