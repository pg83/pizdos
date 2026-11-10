#pragma once

/*
 * Shared vocabulary of every public header. A function marked __KERNEL is
 * imported from the kernel's "kernel" module under its own name; everything
 * else is defined inside the process (crt, stdio, libm and the rest of lib/c).
 * A header's __INLINE definition is inlined at every call and never emitted:
 * lib/c defines the function itself once, which a pointer to it reaches.
 * The ABI is wasm32 with a 64-bit long double, 64-bit time_t, off_t, ino_t
 * and dev_t; struct layouts below are what the kernel reads and writes.
 */

#ifdef __cplusplus
#define __BEGIN_DECLS extern "C" {
#define __END_DECLS }
#define __restrict __restrict__
#define __NOEXCEPT noexcept
#else
#define __BEGIN_DECLS
#define __END_DECLS
#define __restrict restrict
#define __NOEXCEPT
#endif

#define __KERNEL(name) __attribute__((__import_module__("kernel"), __import_name__(#name)))
#define __INLINE extern __inline __attribute__((__gnu_inline__, __always_inline__))
#define __NORETURN __attribute__((__noreturn__))
#define __RETURNS_TWICE __attribute__((__returns_twice__))
#define __PRINTF(f, a) __attribute__((__format__(__printf__, f, a)))
#define __SCANF(f, a) __attribute__((__format__(__scanf__, f, a)))

#ifndef NULL
#ifdef __cplusplus
#define NULL 0L
#else
#define NULL ((void*)0)
#endif
#endif

typedef __SIZE_TYPE__ size_t;
typedef long ssize_t;
typedef __PTRDIFF_TYPE__ ptrdiff_t;
typedef __INTPTR_TYPE__ intptr_t;
typedef __UINTPTR_TYPE__ uintptr_t;
typedef __INTMAX_TYPE__ intmax_t;
typedef __UINTMAX_TYPE__ uintmax_t;
typedef __WINT_TYPE__ wint_t;
typedef __builtin_va_list va_list;
typedef __builtin_va_list __gnuc_va_list;

#ifndef __cplusplus
typedef __WCHAR_TYPE__ wchar_t;
#endif

typedef long long off_t;
typedef long long off64_t;
typedef long long loff_t;
typedef int pid_t;
typedef unsigned uid_t;
typedef unsigned gid_t;
typedef unsigned id_t;
typedef unsigned mode_t;
typedef unsigned nlink_t;
typedef unsigned long long dev_t;
typedef unsigned long long ino_t;
typedef unsigned long long ino64_t;
typedef long blksize_t;
typedef long long blkcnt_t;
typedef unsigned long long fsblkcnt_t;
typedef unsigned long long fsfilcnt_t;
typedef long long time_t;
typedef long long suseconds_t;
typedef long long clock_t;
typedef int clockid_t;
typedef void* timer_t;
typedef unsigned useconds_t;
typedef unsigned socklen_t;
typedef unsigned short sa_family_t;
typedef int key_t;
typedef unsigned long long rlim_t;
typedef struct __locale_struct* locale_t;

typedef struct {
    unsigned long long __bits;
} sigset_t;

struct timespec {
    time_t tv_sec;
    long tv_nsec;
};

struct timeval {
    time_t tv_sec;
    suseconds_t tv_usec;
};

struct iovec {
    void* iov_base;
    size_t iov_len;
};
