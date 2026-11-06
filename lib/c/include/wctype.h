#pragma once

#include <__base.h>

__BEGIN_DECLS

typedef unsigned long wctype_t;
typedef const int* wctrans_t;

#define WEOF 0xffffffffU

int iswalnum(wint_t) __KERNEL(iswalnum);
int iswalpha(wint_t) __KERNEL(iswalpha);
int iswblank(wint_t) __KERNEL(iswblank);
int iswcntrl(wint_t) __KERNEL(iswcntrl);
int iswdigit(wint_t) __KERNEL(iswdigit);
int iswgraph(wint_t) __KERNEL(iswgraph);
int iswlower(wint_t) __KERNEL(iswlower);
int iswprint(wint_t) __KERNEL(iswprint);
int iswpunct(wint_t) __KERNEL(iswpunct);
int iswspace(wint_t) __KERNEL(iswspace);
int iswupper(wint_t) __KERNEL(iswupper);
int iswxdigit(wint_t) __KERNEL(iswxdigit);
int iswctype(wint_t, wctype_t) __KERNEL(iswctype);
wctype_t wctype(const char*) __KERNEL(wctype);
wint_t towlower(wint_t) __KERNEL(towlower);
wint_t towupper(wint_t) __KERNEL(towupper);
wint_t towctrans(wint_t, wctrans_t) __KERNEL(towctrans);
wctrans_t wctrans(const char*) __KERNEL(wctrans);

__END_DECLS
