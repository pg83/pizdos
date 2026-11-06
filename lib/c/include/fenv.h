#pragma once

#include <__base.h>

__BEGIN_DECLS

#define FE_ALL_EXCEPT 0
#define FE_TONEAREST 0

typedef unsigned long fexcept_t;

typedef struct {
    unsigned long __cw;
} fenv_t;

#define FE_DFL_ENV ((const fenv_t*)-1)

int feclearexcept(int);
int fegetexceptflag(fexcept_t*, int);
int feraiseexcept(int);
int fesetexceptflag(const fexcept_t*, int);
int fetestexcept(int);
int fegetround(void);
int fesetround(int);
int fegetenv(fenv_t*);
int feholdexcept(fenv_t*);
int fesetenv(const fenv_t*);
int feupdateenv(const fenv_t*);

__END_DECLS
