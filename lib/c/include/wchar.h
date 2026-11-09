#pragma once

#include <__base.h>

__BEGIN_DECLS

typedef struct _IO_FILE FILE;

typedef struct {
    unsigned __opaque1;
    unsigned __opaque2;
} mbstate_t;

#define WEOF 0xffffffffU
#define WCHAR_MIN (-1 - __WCHAR_MAX__)
#define WCHAR_MAX __WCHAR_MAX__

wint_t btowc(int);
int wctob(wint_t);
int mbsinit(const mbstate_t*);
size_t mbrlen(const char* __restrict, size_t, mbstate_t* __restrict);
size_t mbrtowc(wchar_t* __restrict, const char* __restrict, size_t, mbstate_t* __restrict);
size_t wcrtomb(char* __restrict, wchar_t, mbstate_t* __restrict);
size_t mbsrtowcs(wchar_t* __restrict, const char** __restrict, size_t, mbstate_t* __restrict);
size_t wcsrtombs(char* __restrict, const wchar_t** __restrict, size_t, mbstate_t* __restrict);
size_t mbsnrtowcs(wchar_t* __restrict, const char** __restrict, size_t, size_t, mbstate_t* __restrict);
size_t wcsnrtombs(char* __restrict, const wchar_t** __restrict, size_t, size_t, mbstate_t* __restrict);
long wcstol(const wchar_t* __restrict, wchar_t** __restrict, int);
unsigned long wcstoul(const wchar_t* __restrict, wchar_t** __restrict, int);
long long wcstoll(const wchar_t* __restrict, wchar_t** __restrict, int);
unsigned long long wcstoull(const wchar_t* __restrict, wchar_t** __restrict, int);
float wcstof(const wchar_t* __restrict, wchar_t** __restrict);
double wcstod(const wchar_t* __restrict, wchar_t** __restrict);
long double wcstold(const wchar_t* __restrict, wchar_t** __restrict);

size_t wcslen(const wchar_t*) __KERNEL(wcslen);
size_t wcsnlen(const wchar_t*, size_t) __KERNEL(wcsnlen);
wchar_t* wcscpy(wchar_t* __restrict, const wchar_t* __restrict) __KERNEL(wcscpy);
wchar_t* wcsncpy(wchar_t* __restrict, const wchar_t* __restrict, size_t) __KERNEL(wcsncpy);
wchar_t* wcscat(wchar_t* __restrict, const wchar_t* __restrict) __KERNEL(wcscat);
int wcscmp(const wchar_t*, const wchar_t*) __KERNEL(wcscmp);
int wcsncmp(const wchar_t*, const wchar_t*, size_t) __KERNEL(wcsncmp);
int wcscoll(const wchar_t*, const wchar_t*) __KERNEL(wcscoll);
wchar_t* wcschr(const wchar_t*, wchar_t) __KERNEL(wcschr);
wchar_t* wcsrchr(const wchar_t*, wchar_t) __KERNEL(wcsrchr);
wchar_t* wcsstr(const wchar_t* __restrict, const wchar_t* __restrict) __KERNEL(wcsstr);
wchar_t* wcsdup(const wchar_t*) __KERNEL(wcsdup);
wchar_t* wmemchr(const wchar_t*, wchar_t, size_t) __KERNEL(wmemchr);
int wmemcmp(const wchar_t*, const wchar_t*, size_t) __KERNEL(wmemcmp);
wchar_t* wmemcpy(wchar_t* __restrict, const wchar_t* __restrict, size_t) __KERNEL(wmemcpy);
wchar_t* wmemmove(wchar_t*, const wchar_t*, size_t) __KERNEL(wmemmove);
wchar_t* wmemset(wchar_t*, wchar_t, size_t) __KERNEL(wmemset);
int wcwidth(wchar_t) __KERNEL(wcwidth);
int wcswidth(const wchar_t*, size_t) __KERNEL(wcswidth);

__END_DECLS
