//@ compile-flags: -g -D_GNU_SOURCE -fno-builtin
//@ stdout: "allocation runs: ok\n"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void verify(const unsigned char* block, size_t size, unsigned char value) {
    assert(block);
    for (size_t i = 0; i < size; ++i) {
        assert(block[i] == value);
    }
}

int main() {
#ifdef __wasm__
    // Already backed linear memory must remain usable by later allocations.
    assert(__builtin_wasm_memory_grow(0, 256) != size_t(-1));
#endif
    constexpr size_t segment = 2 * 1024 * 1024;
    const size_t sizes[] = {
        segment - 1, segment + 1, 2 * segment, 2 * segment + 65537,
        3 * segment + 17, segment + 65535, 2 * segment - 1, segment + 131073,
    };
    unsigned char* blocks[8];
    for (unsigned pass = 0; pass < 3; ++pass) {
        for (unsigned i = 0; i < 8; ++i) {
            blocks[i] = static_cast<unsigned char*>(malloc(sizes[i]));
            assert(blocks[i]);
            memset(blocks[i], 31 + i, sizes[i]);
        }
        for (unsigned i = 0; i < 8; ++i) {
            verify(blocks[i], sizes[i], 31 + i);
        }
        for (unsigned i = 0; i < 8; i += 2) {
            free(blocks[i]);
        }
        for (unsigned i = 1; i < 8; i += 2) {
            const size_t grown = sizes[i] + 3 * segment + 1;
            blocks[i] = static_cast<unsigned char*>(realloc(blocks[i], grown));
            verify(blocks[i], sizes[i], 31 + i);
            memset(blocks[i] + sizes[i], 91 + i, grown - sizes[i]);
            verify(blocks[i] + sizes[i], grown - sizes[i], 91 + i);
        }
        for (unsigned i = 0; i < 8; i += 2) {
            blocks[i] = static_cast<unsigned char*>(calloc(sizes[i], 1));
            verify(blocks[i], sizes[i], 0);
            memset(blocks[i], 31 + i, sizes[i]);
        }
        for (unsigned i = 0; i < 8; ++i) {
            verify(blocks[i], sizes[i], 31 + i);
            blocks[i] = static_cast<unsigned char*>(realloc(blocks[i], sizes[i] / 2));
            verify(blocks[i], sizes[i] / 2, 31 + i);
        }
        for (unsigned i = 0; i < 8; ++i) {
            free(blocks[(i * 5) % 8]);
        }
    }
    puts("allocation runs: ok");
}
