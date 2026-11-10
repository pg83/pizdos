#pragma once

#include <__base.h>

__BEGIN_DECLS

typedef struct _IO_FILE FILE;

struct tm;

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

size_t wcslen(const wchar_t*);
size_t wcsnlen(const wchar_t*, size_t);
wchar_t* wcscpy(wchar_t* __restrict, const wchar_t* __restrict);
wchar_t* wcsncpy(wchar_t* __restrict, const wchar_t* __restrict, size_t);
wchar_t* wcscat(wchar_t* __restrict, const wchar_t* __restrict);
int wcscmp(const wchar_t*, const wchar_t*);
int wcsncmp(const wchar_t*, const wchar_t*, size_t);
int wcscoll(const wchar_t*, const wchar_t*);
wchar_t* wcschr(const wchar_t*, wchar_t);
wchar_t* wcsrchr(const wchar_t*, wchar_t);
wchar_t* wcsstr(const wchar_t* __restrict, const wchar_t* __restrict);
wchar_t* wcspbrk(const wchar_t*, const wchar_t*);
size_t wcsspn(const wchar_t*, const wchar_t*);
size_t wcscspn(const wchar_t*, const wchar_t*);
wchar_t* wcstok(wchar_t* __restrict, const wchar_t* __restrict, wchar_t** __restrict);
size_t wcsxfrm(wchar_t* __restrict, const wchar_t* __restrict, size_t);
size_t wcsftime(wchar_t* __restrict, size_t, const wchar_t* __restrict, const struct tm* __restrict);
wchar_t* wcsdup(const wchar_t*);
wchar_t* wmemchr(const wchar_t*, wchar_t, size_t);
int wmemcmp(const wchar_t*, const wchar_t*, size_t);
wchar_t* wmemcpy(wchar_t* __restrict, const wchar_t* __restrict, size_t);
wchar_t* wmemmove(wchar_t*, const wchar_t*, size_t);
wchar_t* wmemset(wchar_t*, wchar_t, size_t);
int wcwidth(wchar_t);
int wcswidth(const wchar_t*, size_t);

__END_DECLS
