//@ compile-flags: -g -D_GNU_SOURCE -fno-builtin
//@ stdout: "allocation churn: ok\n"

#include <assert.h>
#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static unsigned char* blocks[16384];
static uintptr_t ordered[16384];

static unsigned char marker(size_t index, unsigned generation) {
    return (index * 37 + generation * 73) % 251 + 1;
}

static void check(size_t count, size_t size, unsigned generation) {
    for (size_t i = 0; i < count; ++i) {
        assert(blocks[i] && malloc_usable_size(blocks[i]) >= size);
        const unsigned value = marker(i, i % 3 == 0 ? generation : 0);
        for (size_t j = 0; j < size; ++j) {
            assert(blocks[i][j] == value);
        }
        ordered[i] = uintptr_t(blocks[i]);
    }
    qsort(ordered, count, sizeof(ordered[0]), [](const void* a, const void* b) {
        const auto x = *static_cast<const uintptr_t*>(a);
        const auto y = *static_cast<const uintptr_t*>(b);
        return (x > y) - (x < y);
    });
    for (size_t i = 1; i < count; ++i) {
        assert(ordered[i - 1] + size <= ordered[i]);
    }
}

int main() {
    const size_t sizes[] = {
        1, 15, 16, 17, 31, 32, 33, 63, 64, 65, 95, 127, 128, 129,
        159, 191, 223, 255, 256, 257, 319, 383, 447, 511, 512, 513,
        639, 767, 895, 1023, 1024, 1025, 1279, 1535, 1791, 2047,
        2048, 2049, 2559, 3071, 3583, 4095, 4096, 4097, 5119, 6143,
        7167, 8191, 8192, 8193, 10239, 12287, 14335, 16383, 16384,
    };
    for (size_t size : sizes) {
        size_t count = 64;
        while (count * (size < 16 ? 16 : size) < 262144) {
            count *= 2;
        }
        for (size_t i = 0; i < count; ++i) {
            blocks[i] = static_cast<unsigned char*>(malloc(size));
            assert(blocks[i]);
            memset(blocks[i], marker(i, 0), size);
        }
        check(count, size, 0);
        for (size_t i = 0; i < count; i += 3) {
            free(blocks[i]);
        }
        for (size_t i = 0; i < count; i += 3) {
            blocks[i] = static_cast<unsigned char*>(calloc(size, 1));
            assert(blocks[i]);
            for (size_t j = 0; j < size; ++j) {
                assert(blocks[i][j] == 0);
            }
            memset(blocks[i], marker(i, 1), size);
        }
        check(count, size, 1);
        for (size_t i = 0; i < count; ++i) {
            free(blocks[(i * 2053) & (count - 1)]);
        }
    }
    const int page = getpagesize();
    assert(page > 0 && (page & (page - 1)) == 0);
#ifdef __wasm__
    assert(mallopt(M_TRIM_THRESHOLD, 131072) == 1);
#endif
    puts("allocation churn: ok");
}
