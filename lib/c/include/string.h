#pragma once

#include <__base.h>

__BEGIN_DECLS

void* memcpy(void* __restrict, const void* __restrict, size_t);
void* memmove(void*, const void*, size_t);
void* memset(void*, int, size_t);

int memcmp(const void*, const void*, size_t) __KERNEL(memcmp);
void* memchr(const void*, int, size_t) __KERNEL(memchr);
void* memrchr(const void*, int, size_t) __KERNEL(memrchr);
void* memmem(const void*, size_t, const void*, size_t) __KERNEL(memmem);
void* mempcpy(void* __restrict, const void* __restrict, size_t) __KERNEL(mempcpy);
void* memccpy(void* __restrict, const void* __restrict, int, size_t) __KERNEL(memccpy);
void* rawmemchr(const void*, int) __KERNEL(rawmemchr);

size_t strlen(const char*) __KERNEL(strlen);
size_t strnlen(const char*, size_t) __KERNEL(strnlen);
char* strcpy(char* __restrict, const char* __restrict) __KERNEL(strcpy);
char* strncpy(char* __restrict, const char* __restrict, size_t) __KERNEL(strncpy);
char* stpcpy(char* __restrict, const char* __restrict) __KERNEL(stpcpy);
char* stpncpy(char* __restrict, const char* __restrict, size_t) __KERNEL(stpncpy);
char* strcat(char* __restrict, const char* __restrict) __KERNEL(strcat);
char* strncat(char* __restrict, const char* __restrict, size_t) __KERNEL(strncat);
size_t strlcpy(char*, const char*, size_t) __KERNEL(strlcpy);
size_t strlcat(char*, const char*, size_t) __KERNEL(strlcat);

int strcmp(const char*, const char*) __KERNEL(strcmp);
int strncmp(const char*, const char*, size_t) __KERNEL(strncmp);
int strcoll(const char*, const char*) __KERNEL(strcoll);
size_t strxfrm(char* __restrict, const char* __restrict, size_t) __KERNEL(strxfrm);
int strverscmp(const char*, const char*) __KERNEL(strverscmp);

char* strchr(const char*, int) __KERNEL(strchr);
char* strrchr(const char*, int) __KERNEL(strrchr);
char* strchrnul(const char*, int) __KERNEL(strchrnul);
char* strstr(const char*, const char*) __KERNEL(strstr);
char* strcasestr(const char*, const char*) __KERNEL(strcasestr);
size_t strspn(const char*, const char*) __KERNEL(strspn);
size_t strcspn(const char*, const char*) __KERNEL(strcspn);
char* strpbrk(const char*, const char*) __KERNEL(strpbrk);
char* strtok(char* __restrict, const char* __restrict) __KERNEL(strtok);
char* strtok_r(char* __restrict, const char* __restrict, char** __restrict) __KERNEL(strtok_r);
char* strsep(char**, const char*) __KERNEL(strsep);

char* strdup(const char*) __KERNEL(strdup);
char* strndup(const char*, size_t) __KERNEL(strndup);

char* strerror(int) __KERNEL(strerror);
int strerror_r(int, char*, size_t) __KERNEL(strerror_r);
char* strsignal(int) __KERNEL(strsignal);

void explicit_bzero(void*, size_t) __KERNEL(explicit_bzero);

__END_DECLS

#include <strings.h>
