#include <string.h>
#include <strings.h>

/*
 * The three bulk operations stay in the process: they lower to the
 * memory.copy and memory.fill instructions, which the engine executes
 * natively, and the compiler emits calls to them on its own.
 */
void* memcpy(void* __restrict dst, const void* __restrict src, size_t n) {
    return __builtin_memcpy(dst, src, n);
}

void* memmove(void* dst, const void* src, size_t n) {
    return __builtin_memmove(dst, src, n);
}

void* memset(void* dst, int c, size_t n) {
    return __builtin_memset(dst, c, n);
}

int bcmp(const void* a, const void* b, size_t n) {
    return memcmp(a, b, n);
}
