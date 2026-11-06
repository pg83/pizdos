#pragma once

#include <__base.h>

__BEGIN_DECLS

int isalnum(int) __KERNEL(isalnum);
int isalpha(int) __KERNEL(isalpha);
int isascii(int) __KERNEL(isascii);
int isblank(int) __KERNEL(isblank);
int iscntrl(int) __KERNEL(iscntrl);
int isdigit(int) __KERNEL(isdigit);
int isgraph(int) __KERNEL(isgraph);
int islower(int) __KERNEL(islower);
int isprint(int) __KERNEL(isprint);
int ispunct(int) __KERNEL(ispunct);
int isspace(int) __KERNEL(isspace);
int isupper(int) __KERNEL(isupper);
int isxdigit(int) __KERNEL(isxdigit);
int tolower(int) __KERNEL(tolower);
int toupper(int) __KERNEL(toupper);
int toascii(int) __KERNEL(toascii);

__END_DECLS
