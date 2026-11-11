//@ host: false
//@ compile-flags: -g -D_GNU_SOURCE -fno-builtin
//@ max-memory: 4194304
//@ stdout: "memory exhaustion: ok\n"

#include <assert.h>
#include <errno.h>
#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static void* large[128];
static void* small[8192];

int main() {
    auto* preserved = static_cast<unsigned char*>(malloc(16));
    assert(preserved);
    memset(preserved, 0x57, 16);
    size_t count = 0;
    while (count < 128 && (large[count] = malloc(65536))) {
        memset(large[count], 0x33, 65536);
        ++count;
    }
    assert(count > 0 && count < 128 && errno == ENOMEM);
    size_t tiny = 0;
    while (tiny < 8192 && (small[tiny] = malloc(16))) {
        ++tiny;
    }
    assert(tiny < 8192 && errno == ENOMEM);
    assert(calloc(1, 65536) == nullptr && errno == ENOMEM);
    assert(realloc(nullptr, 65536) == nullptr && errno == ENOMEM);
    assert(realloc(preserved, 65536) == nullptr && errno == ENOMEM);
    assert(reallocarray(preserved, 2, 65536) == nullptr && errno == ENOMEM);
    assert(malloc(8192) == nullptr && errno == ENOMEM);
    for (unsigned i = 0; i < 16; ++i) {
        assert(preserved[i] == 0x57);
    }
    assert(aligned_alloc(131072, 131072) == nullptr && errno == ENOMEM);
    assert(memalign(131072, 1) == nullptr && errno == ENOMEM);
    assert(valloc(65536) == nullptr && errno == ENOMEM);
    void* aligned = preserved;
    assert(posix_memalign(&aligned, 131072, 131072) == ENOMEM && aligned == preserved);
    assert(mmap(nullptr, 65536, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED && errno == ENOMEM);
    assert(mmap(nullptr, SIZE_MAX, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED && errno == ENOMEM);
    assert(strdup("x") == nullptr && errno == ENOMEM);
    assert(getcwd(nullptr, 0) == nullptr && errno == ENOMEM);
    assert(get_current_dir_name() == nullptr && errno == ENOMEM);
    assert(canonicalize_file_name("/") == nullptr && errno == ENOMEM);

    free(large[--count]);
    auto* recovered = static_cast<unsigned char*>(calloc(1, 65536));
    assert(recovered && recovered[0] == 0 && recovered[65535] == 0);
    free(recovered);
    for (size_t i = 0; i < count; ++i) {
        assert(static_cast<unsigned char*>(large[i])[65535] == 0x33);
        free(large[i]);
    }
    for (size_t i = 0; i < tiny; ++i) {
        free(small[i]);
    }
    assert(realloc(preserved, 0) == nullptr);
    recovered = static_cast<unsigned char*>(malloc(1024 * 1024));
    assert(recovered);
    recovered[1024 * 1024 - 1] = 0x75;
    assert(recovered[1024 * 1024 - 1] == 0x75);
    free(recovered);
    puts("memory exhaustion: ok");
}
