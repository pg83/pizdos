//@ compile-flags: -g -D_GNU_SOURCE -fno-builtin
//@ stdout: "intrinsic results: ok\n"

#include <assert.h>
#include <stdio.h>
#include <string.h>

__attribute__((noinline)) static void* copied(void* dst, const void* src, size_t size) {
    return memcpy(dst, src, size);
}

__attribute__((noinline)) static void* moved(void* dst, const void* src, size_t size) {
    return memmove(dst, src, size);
}

__attribute__((noinline)) static void* filled(void* dst, int value, size_t size) {
    return memset(dst, value, size);
}

__attribute__((noinline)) static char* copiedString(char* dst, const char* src) {
    return strcpy(dst, src);
}

__attribute__((noinline)) static char* appended(char* dst, const char* src) {
    return strcat(dst, src);
}

int main() {
    char data[64]{};
    void* (*volatile copy)(void*, const void*, size_t) = copied;
    void* (*volatile move)(void*, const void*, size_t) = moved;
    void* (*volatile fill)(void*, int, size_t) = filled;
    char* (*volatile stringCopy)(char*, const char*) = copiedString;
    char* (*volatile append)(char*, const char*) = appended;
    assert(copy(data, "abcd", 5) == data && strcmp(data, "abcd") == 0);
    assert(move(data + 1, data, 5) == data + 1 && strcmp(data, "aabcd") == 0);
    assert(fill(data, 'x', 3) == data && strcmp(data, "xxxcd") == 0);
    assert(stringCopy(data + 2, "hello") == data + 2 && strcmp(data + 2, "hello") == 0);
    assert(append(data + 2, "!") == data + 2 && strcmp(data + 2, "hello!") == 0);
    // Unused results still require safe, observable calls through imports.
    (void)memcmp(data, "xxx", 3);
    (void)strcmp(data, "xxx");
    (void)memchr(data, 'x', sizeof(data));
    puts("intrinsic results: ok");
}
