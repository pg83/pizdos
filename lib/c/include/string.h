#pragma once

#include <__base.h>

__BEGIN_DECLS

void* memcpy(void* __restrict, const void* __restrict, size_t);
void* memmove(void*, const void*, size_t);
void* memset(void*, int, size_t);
int memcmp(const void*, const void*, size_t);
void* memchr(const void*, int, size_t);
void* memrchr(const void*, int, size_t) __KERNEL(memrchr);
void* memmem(const void*, size_t, const void*, size_t) __KERNEL(memmem);
void* mempcpy(void* __restrict, const void* __restrict, size_t) __KERNEL(mempcpy);
void* memccpy(void* __restrict, const void* __restrict, int, size_t) __KERNEL(memccpy);
void* rawmemchr(const void*, int) __KERNEL(rawmemchr);

size_t strlen(const char*);
size_t strnlen(const char*, size_t) __KERNEL(strnlen);
char* strcpy(char* __restrict, const char* __restrict);
char* strncpy(char* __restrict, const char* __restrict, size_t) __KERNEL(strncpy);
char* stpcpy(char* __restrict, const char* __restrict) __KERNEL(stpcpy);
char* stpncpy(char* __restrict, const char* __restrict, size_t) __KERNEL(stpncpy);
char* strcat(char* __restrict, const char* __restrict);
char* strncat(char* __restrict, const char* __restrict, size_t) __KERNEL(strncat);
size_t strlcpy(char*, const char*, size_t) __KERNEL(strlcpy);
size_t strlcat(char*, const char*, size_t) __KERNEL(strlcat);

int strcmp(const char*, const char*);
int strncmp(const char*, const char*, size_t) __KERNEL(strncmp);
int strcoll(const char*, const char*) __KERNEL(strcoll);
size_t strxfrm(char* __restrict, const char* __restrict, size_t) __KERNEL(strxfrm);
int strverscmp(const char*, const char*) __KERNEL(strverscmp);

char* strchr(const char*, int) __KERNEL(strchr);
char* strrchr(const char*, int) __KERNEL(strrchr);
char* strchrnul(const char*, int) __KERNEL(strchrnul);
char* strstr(const char*, const char*) __KERNEL(strstr);
char* strcasestr(const char*, const char*) __KERNEL(strcasestr);
size_t strspn(const char*, const char*);
size_t strcspn(const char*, const char*);
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

/*
 * These are lib/c's functions over imports the compiler compiles at each
 * call, as the kernel would do them: the ranges and strings checked, a
 * trap past the memory's end. Inline here, so that a call is the import's
 * call; defined in mem.c for a pointer and a call without this header.
 */
void* __kernel_memcpy(void* __restrict, const void* __restrict, size_t) __KERNEL(__kernel_memcpy);
void* __kernel_memmove(void*, const void*, size_t) __KERNEL(__kernel_memmove);
void* __kernel_memset(void*, int, size_t) __KERNEL(__kernel_memset);
int __kernel_memcmp(const void*, const void*, size_t) __KERNEL(__kernel_memcmp);
void* __kernel_memchr(const void*, int, size_t) __KERNEL(__kernel_memchr);
size_t __kernel_strlen(const char*) __KERNEL(__kernel_strlen);
char* __kernel_strcpy(char* __restrict, const char* __restrict) __KERNEL(__kernel_strcpy);
char* __kernel_strcat(char* __restrict, const char* __restrict) __KERNEL(__kernel_strcat);
int __kernel_strcmp(const char*, const char*) __KERNEL(__kernel_strcmp);
size_t __kernel_strspn(const char*, const char*) __KERNEL(__kernel_strspn);
size_t __kernel_strcspn(const char*, const char*) __KERNEL(__kernel_strcspn);

__INLINE void* memcpy(void* __restrict __d, const void* __restrict __s, size_t __n) {
    return __kernel_memcpy(__d, __s, __n);
}

__INLINE void* memmove(void* __d, const void* __s, size_t __n) {
    return __kernel_memmove(__d, __s, __n);
}

__INLINE void* memset(void* __d, int __c, size_t __n) {
    return __kernel_memset(__d, __c, __n);
}

__INLINE int memcmp(const void* __a, const void* __b, size_t __n) {
    return __kernel_memcmp(__a, __b, __n);
}

__INLINE void* memchr(const void* __s, int __c, size_t __n) {
    return __kernel_memchr(__s, __c, __n);
}

__INLINE size_t strlen(const char* __s) {
    return __kernel_strlen(__s);
}

__INLINE char* strcpy(char* __restrict __d, const char* __restrict __s) {
    return __kernel_strcpy(__d, __s);
}

__INLINE char* strcat(char* __restrict __d, const char* __restrict __s) {
    return __kernel_strcat(__d, __s);
}

__INLINE int strcmp(const char* __a, const char* __b) {
    return __kernel_strcmp(__a, __b);
}

__INLINE size_t strspn(const char* __s, const char* __set) {
    return __kernel_strspn(__s, __set);
}

__INLINE size_t strcspn(const char* __s, const char* __set) {
    return __kernel_strcspn(__s, __set);
}

__END_DECLS

#include <strings.h>
