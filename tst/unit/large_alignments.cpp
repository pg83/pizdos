//@ compile-flags: -g -D_GNU_SOURCE -fno-builtin
//@ stdout: "large alignments: ok\n"

#include <assert.h>
#include <errno.h>
#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check(const void* p, size_t n, unsigned char byte) {
    const auto* data = static_cast<const unsigned char*>(p);
    for (size_t i = 0; i != n; ++i) {
        assert(data[i] == byte);
    }
}

int main() {
    const size_t sizes[] = {1, 17, 4095, 16385, 65537};
    for (size_t alignment = 16; alignment <= 1048576; alignment *= 2) {
        for (size_t n : sizes) {
            void* first = nullptr;
            assert(posix_memalign(&first, alignment, n) == 0 && first);
            assert(uintptr_t(first) % alignment == 0 && malloc_usable_size(first) >= n);
            memset(first, 0x37, n);
            void* second = memalign(alignment, n);
            assert(second && uintptr_t(second) % alignment == 0 && malloc_usable_size(second) >= n);
            memset(second, 0xa5, n);
            check(first, n, 0x37);
            check(second, n, 0xa5);
            first = realloc(first, n + 37);
            assert(first && malloc_usable_size(first) >= n + 37);
            check(first, n, 0x37);
            memset(first, 0x71, n + 37);
            check(second, n, 0xa5);
            check(first, n + 37, 0x71);
            free(second);
            free(first);

            size_t rounded = (n + alignment - 1) & ~(alignment - 1);
            first = aligned_alloc(alignment, rounded);
            assert(first && uintptr_t(first) % alignment == 0 && malloc_usable_size(first) >= rounded);
            memset(first, 0x28, rounded);
            check(first, rounded, 0x28);
            free(first);
        }
        void* empty = nullptr;
        assert(posix_memalign(&empty, alignment, 0) == 0);
        if (empty) {
            assert(uintptr_t(empty) % alignment == 0);
            free(empty);
        }
    }
    void* unchanged = reinterpret_cast<void*>(uintptr_t(1234));
    assert(posix_memalign(&unchanged, 3, 64) == EINVAL && uintptr_t(unchanged) == 1234);
    assert(posix_memalign(&unchanged, 0, 64) == EINVAL && uintptr_t(unchanged) == 1234);
    puts("large alignments: ok");
}
