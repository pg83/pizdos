#pragma once

#include <__base.h>
#include <alloca.h>

__BEGIN_DECLS

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 0x7fffffff
#define MB_CUR_MAX ((size_t)4)

typedef struct {
    int quot;
    int rem;
} div_t;

typedef struct {
    long quot;
    long rem;
} ldiv_t;

typedef struct {
    long long quot;
    long long rem;
} lldiv_t;

void* malloc(size_t);
void* calloc(size_t, size_t);
void* realloc(void*, size_t);
void* reallocarray(void*, size_t, size_t);
void free(void*);
void* aligned_alloc(size_t, size_t);
int posix_memalign(void**, size_t, size_t);
void* valloc(size_t);

int atoi(const char*);
long atol(const char*);
long long atoll(const char*);
double atof(const char*);
double strtod(const char* __restrict, char** __restrict);
float strtof(const char* __restrict, char** __restrict);
long double strtold(const char* __restrict, char** __restrict);
long strtol(const char* __restrict, char** __restrict, int);
unsigned long strtoul(const char* __restrict, char** __restrict, int);
long long strtoll(const char* __restrict, char** __restrict, int);
unsigned long long strtoull(const char* __restrict, char** __restrict, int);

void qsort(void*, size_t, size_t, int (*)(const void*, const void*));
void qsort_r(void*, size_t, size_t, int (*)(const void*, const void*, void*), void*);
void* bsearch(const void*, const void*, size_t, size_t, int (*)(const void*, const void*));

int mblen(const char*, size_t);
int mbtowc(wchar_t* __restrict, const char* __restrict, size_t);
int wctomb(char*, wchar_t);
size_t mbstowcs(wchar_t* __restrict, const char* __restrict, size_t);
size_t wcstombs(char* __restrict, const wchar_t* __restrict, size_t);

int atexit(void (*)(void));
__NORETURN void exit(int);
__NORETURN void _Exit(int);
__NORETURN void abort(void);

char* getenv(const char*);
char* secure_getenv(const char*);
int setenv(const char*, const char*, int);
int unsetenv(const char*);
int putenv(char*);
int clearenv(void);
int system(const char*);
char* realpath(const char* __restrict, char* __restrict);
char* canonicalize_file_name(const char*);

char* mktemp(char*);
int mkstemp(char*);
int mkostemp(char*, int);
int mkstemps(char*, int);
char* mkdtemp(char*);

int abs(int);
long labs(long);
long long llabs(long long);
div_t div(int, int);
ldiv_t ldiv(long, long);
lldiv_t lldiv(long long, long long);

int rand(void);
void srand(unsigned);
int rand_r(unsigned*);
long random(void);
void srandom(unsigned);
char* initstate(unsigned, char*, size_t);
char* setstate(char*);

int posix_openpt(int);
int grantpt(int);
int unlockpt(int);
char* ptsname(int);
int ptsname_r(int, char*, size_t);
int getloadavg(double*, int);
int getsubopt(char**, char* const*, char**);

/*
 * lib/c's over the kernel's imports, inline so that a call is the
 * import's call; mem.c defines them for a pointer and a call without
 * this header.
 */
void* __kernel_malloc(size_t) __KERNEL(malloc);
void* __kernel_calloc(size_t, size_t) __KERNEL(calloc);
void* __kernel_realloc(void*, size_t) __KERNEL(realloc);
void __kernel_free(void*) __KERNEL(free);

__INLINE void* malloc(size_t __a) {
    return __kernel_malloc(__a);
}

__INLINE void* calloc(size_t __a, size_t __b) {
    return __kernel_calloc(__a, __b);
}

__INLINE void* realloc(void* __a, size_t __b) {
    return __kernel_realloc(__a, __b);
}

__INLINE void free(void* __a) {
    __kernel_free(__a);
}

__END_DECLS
