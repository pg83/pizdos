#pragma once

#include <__base.h>

__BEGIN_DECLS

typedef struct {
    unsigned long long __regs[8];
    int __mask_saved;
    sigset_t __mask;
} jmp_buf[1];

typedef jmp_buf sigjmp_buf;

int setjmp(jmp_buf) __RETURNS_TWICE;
int _setjmp(jmp_buf) __RETURNS_TWICE;
int sigsetjmp(sigjmp_buf, int) __RETURNS_TWICE;
__NORETURN void longjmp(jmp_buf, int);
__NORETURN void _longjmp(jmp_buf, int);
__NORETURN void siglongjmp(sigjmp_buf, int);

/*
 * Inline only: the import's call is in the caller's frame, which a
 * later return lands in. There is no definition to point to. The buffer
 * comes with its size, as every structure the kernel takes does.
 */
int __kernel_setjmp(jmp_buf, size_t) __RETURNS_TWICE __KERNEL(setjmp);
int __kernel__setjmp(jmp_buf, size_t) __RETURNS_TWICE __KERNEL(_setjmp);
int __kernel_sigsetjmp(sigjmp_buf, size_t, int) __RETURNS_TWICE __KERNEL(sigsetjmp);

__INLINE int setjmp(jmp_buf __a) {
    return __kernel_setjmp(__a, sizeof(jmp_buf));
}

__INLINE int _setjmp(jmp_buf __a) {
    return __kernel__setjmp(__a, sizeof(jmp_buf));
}

__INLINE int sigsetjmp(sigjmp_buf __a, int __b) {
    return __kernel_sigsetjmp(__a, sizeof(sigjmp_buf), __b);
}

__END_DECLS
