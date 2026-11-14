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

void* mmap(void*, size_t, int, int, int, off_t);
int munmap(void*, size_t);
int mprotect(void*, size_t, int);
int msync(void*, size_t, int);
int madvise(void*, size_t, int);
int mlock(const void*, size_t);
int munlock(const void*, size_t);
int mlockall(int);
int munlockall(void);

/*
 * memfd_create makes a file of the length bytes of the caller's memory at
 * addr, open in the access mode of flags (O_RDONLY, O_WRONLY, O_RDWR, and
 * O_CLOEXEC): a descriptor of it, passed to another process, lets that
 * process read or write that memory, a read straight into its own memory
 * and a write straight from it. The file keeps the memory while it is
 * open, past the creator's end.
 */
int memfd_create(void*, size_t, int);

__END_DECLS
