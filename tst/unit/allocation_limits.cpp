//@ compile-flags: -g -D_GNU_SOURCE -fno-builtin
//@ stdout: "allocation limits: ok\n"

#include <assert.h>
#include <errno.h>
#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main() {
    const size_t sizes[] = {1, 15, 16, 17, 63, 64, 65, 511, 512, 513, 2047, 2048, 2049, 4095, 4096, 4097, 100000};
    for (size_t size : sizes) {
        auto* block = static_cast<unsigned char*>(calloc(size, 1));
        assert(block && malloc_usable_size(block) >= size);
        for (size_t i = 0; i < size; ++i) {
            assert(block[i] == 0);
            block[i] = unsigned(i % 251);
        }
        auto* grown = static_cast<unsigned char*>(realloc(block, size * 2));
        assert(grown);
        for (size_t i = 0; i < size; ++i) {
            assert(grown[i] == i % 251);
        }
        grown[size * 2 - 1] = 7;
        auto* shrunk = static_cast<unsigned char*>(realloc(grown, size));
        assert(shrunk);
        for (size_t i = 0; i < size; ++i) {
            assert(shrunk[i] == i % 251);
        }
        free(shrunk);
    }
    void* zero = malloc(0);
    free(zero);
    free(nullptr);
    void* block = realloc(nullptr, 64);
    assert(block);
    memset(block, 42, 64);
    volatile size_t maximum = SIZE_MAX;
    errno = 0;
    assert(malloc(maximum) == nullptr && errno == ENOMEM);
    assert(calloc(maximum / 2 + 1, 2) == nullptr && errno == ENOMEM);
    assert(realloc(block, maximum) == nullptr && errno == ENOMEM);
    assert(reallocarray(block, maximum, 2) == nullptr && errno == ENOMEM);
    for (unsigned i = 0; i < 64; ++i) {
        assert(static_cast<unsigned char*>(block)[i] == 42);
    }
    free(block);
    block = aligned_alloc(128, 256);
    assert(block && uintptr_t(block) % 128 == 0);
    memset(block, 1, 256);
    free(block);
    for (size_t alignment : sizes) {
        if ((alignment & (alignment - 1)) != 0 || alignment < sizeof(void*)) {
            continue;
        }
        block = nullptr;
        assert(posix_memalign(&block, alignment, 129) == 0 && block && uintptr_t(block) % alignment == 0);
        memset(block, 2, 129);
        free(block);
    }
    block = memalign(65536, 129);
    assert(block && uintptr_t(block) % 65536 == 0);
    memset(block, 3, 129);
    free(block);
    block = valloc(17);
    assert(block && uintptr_t(block) % size_t(sysconf(_SC_PAGESIZE)) == 0);
    memset(block, 4, 17);
    free(block);
    int sentinel;
    block = &sentinel;
    assert(posix_memalign(&block, 3, 16) == EINVAL && block == &sentinel);
    assert(posix_memalign(&block, sizeof(void*) / 2, 16) == EINVAL && block == &sentinel);
    assert(posix_memalign(&block, 32, maximum) == ENOMEM && block == &sentinel);
    assert(malloc_usable_size(nullptr) == 0);
    puts("allocation limits: ok");
}
