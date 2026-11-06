#pragma once

#include <__base.h>

__BEGIN_DECLS

int bcmp(const void*, const void*, size_t);
void bcopy(const void*, void*, size_t) __KERNEL(bcopy);
void bzero(void*, size_t) __KERNEL(bzero);
char* index(const char*, int) __KERNEL(index);
char* rindex(const char*, int) __KERNEL(rindex);
int ffs(int) __KERNEL(ffs);
int ffsl(long) __KERNEL(ffsl);
int ffsll(long long) __KERNEL(ffsll);
int strcasecmp(const char*, const char*) __KERNEL(strcasecmp);
int strncasecmp(const char*, const char*, size_t) __KERNEL(strncasecmp);

__END_DECLS
