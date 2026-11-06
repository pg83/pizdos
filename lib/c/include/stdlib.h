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

void* malloc(size_t) __KERNEL(malloc);
void* calloc(size_t, size_t) __KERNEL(calloc);
void* realloc(void*, size_t) __KERNEL(realloc);
void* reallocarray(void*, size_t, size_t) __KERNEL(reallocarray);
void free(void*) __KERNEL(free);
void* aligned_alloc(size_t, size_t) __KERNEL(aligned_alloc);
int posix_memalign(void**, size_t, size_t) __KERNEL(posix_memalign);
void* valloc(size_t) __KERNEL(valloc);

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
__NORETURN void _Exit(int) __KERNEL(_Exit);
__NORETURN void abort(void) __KERNEL(abort);

char* getenv(const char*) __KERNEL(getenv);
char* secure_getenv(const char*) __KERNEL(secure_getenv);
int setenv(const char*, const char*, int) __KERNEL(setenv);
int unsetenv(const char*) __KERNEL(unsetenv);
int putenv(char*) __KERNEL(putenv);
int clearenv(void) __KERNEL(clearenv);
int system(const char*) __KERNEL(system);
char* realpath(const char* __restrict, char* __restrict) __KERNEL(realpath);
char* canonicalize_file_name(const char*) __KERNEL(canonicalize_file_name);

char* mktemp(char*) __KERNEL(mktemp);
int mkstemp(char*) __KERNEL(mkstemp);
int mkostemp(char*, int) __KERNEL(mkostemp);
int mkstemps(char*, int) __KERNEL(mkstemps);
char* mkdtemp(char*) __KERNEL(mkdtemp);

int abs(int) __KERNEL(abs);
long labs(long) __KERNEL(labs);
long long llabs(long long) __KERNEL(llabs);
div_t div(int, int) __KERNEL(div);
ldiv_t ldiv(long, long) __KERNEL(ldiv);
lldiv_t lldiv(long long, long long) __KERNEL(lldiv);

int rand(void) __KERNEL(rand);
void srand(unsigned) __KERNEL(srand);
int rand_r(unsigned*) __KERNEL(rand_r);
long random(void) __KERNEL(random);
void srandom(unsigned) __KERNEL(srandom);
char* initstate(unsigned, char*, size_t) __KERNEL(initstate);
char* setstate(char*) __KERNEL(setstate);

int posix_openpt(int) __KERNEL(posix_openpt);
int grantpt(int) __KERNEL(grantpt);
int unlockpt(int) __KERNEL(unlockpt);
char* ptsname(int) __KERNEL(ptsname);
int ptsname_r(int, char*, size_t) __KERNEL(ptsname_r);
int getloadavg(double*, int) __KERNEL(getloadavg);
int getsubopt(char**, char* const*, char**) __KERNEL(getsubopt);

__END_DECLS
