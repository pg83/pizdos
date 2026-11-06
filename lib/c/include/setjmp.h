#pragma once

#include <__base.h>

__BEGIN_DECLS

typedef struct {
    unsigned long long __regs[8];
    int __mask_saved;
    sigset_t __mask;
} jmp_buf[1];

typedef jmp_buf sigjmp_buf;

int setjmp(jmp_buf) __RETURNS_TWICE __KERNEL(setjmp);
int _setjmp(jmp_buf) __RETURNS_TWICE __KERNEL(_setjmp);
int sigsetjmp(sigjmp_buf, int) __RETURNS_TWICE __KERNEL(sigsetjmp);
__NORETURN void longjmp(jmp_buf, int) __KERNEL(longjmp);
__NORETURN void _longjmp(jmp_buf, int) __KERNEL(_longjmp);
__NORETURN void siglongjmp(sigjmp_buf, int) __KERNEL(siglongjmp);

__END_DECLS
