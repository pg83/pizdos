#include <stdlib.h>
#include <string.h>
#include <strings.h>

/*
 * The functions stdlib.h and string.h inline, for a pointer to one and a
 * call without the header: the same calls of the imports.
 */
void* malloc(size_t n) {
    return __kernel_malloc(n);
}

void* calloc(size_t count, size_t size) {
    return __kernel_calloc(count, size);
}

void* realloc(void* p, size_t n) {
    return __kernel_realloc(p, n);
}

void free(void* p) {
    __kernel_free(p);
}

void* memcpy(void* restrict d, const void* restrict s, size_t n) {
    return __kernel_memcpy(d, s, n);
}

void* memmove(void* d, const void* s, size_t n) {
    return __kernel_memmove(d, s, n);
}

void* memset(void* d, int c, size_t n) {
    return __kernel_memset(d, c, n);
}

int memcmp(const void* a, const void* b, size_t n) {
    return __kernel_memcmp(a, b, n);
}

void* memchr(const void* s, int c, size_t n) {
    return __kernel_memchr(s, c, n);
}

size_t strlen(const char* s) {
    return __kernel_strlen(s);
}

char* strcpy(char* restrict d, const char* restrict s) {
    return __kernel_strcpy(d, s);
}

char* strcat(char* restrict d, const char* restrict s) {
    return __kernel_strcat(d, s);
}

int strcmp(const char* a, const char* b) {
    return __kernel_strcmp(a, b);
}

size_t strspn(const char* s, const char* set) {
    return __kernel_strspn(s, set);
}

size_t strcspn(const char* s, const char* set) {
    return __kernel_strcspn(s, set);
}

int bcmp(const void* a, const void* b, size_t n) {
    return memcmp(a, b, n);
}
