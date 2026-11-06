#pragma once

#include <__base.h>

__BEGIN_DECLS

#define MAP_FAILED ((void*)-1)

#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4

#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON MAP_ANONYMOUS
#define MAP_NORESERVE 0x4000
#define MAP_POPULATE 0x8000
#define MAP_NONBLOCK 0x10000
#define MAP_STACK 0x20000

#define MS_ASYNC 1
#define MS_INVALIDATE 2
#define MS_SYNC 4

#define MCL_CURRENT 1
#define MCL_FUTURE 2

#define MADV_NORMAL 0
#define MADV_RANDOM 1
#define MADV_SEQUENTIAL 2
#define MADV_WILLNEED 3
#define MADV_DONTNEED 4

void* mmap(void*, size_t, int, int, int, off_t) __KERNEL(mmap);
int munmap(void*, size_t) __KERNEL(munmap);
int mprotect(void*, size_t, int) __KERNEL(mprotect);
int msync(void*, size_t, int) __KERNEL(msync);
int madvise(void*, size_t, int) __KERNEL(madvise);
int mlock(const void*, size_t) __KERNEL(mlock);
int munlock(const void*, size_t) __KERNEL(munlock);
int mlockall(int) __KERNEL(mlockall);
int munlockall(void) __KERNEL(munlockall);

__END_DECLS
