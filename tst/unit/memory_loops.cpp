//@ compile-flags: -g -fno-builtin -fno-unroll-loops -fno-vectorize -fno-slp-vectorize
//@ stdout: "memory loops: ok\n"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

__attribute__((noinline)) static void forward(unsigned char* dst, const unsigned char* src, unsigned n) {
    for (unsigned i = 0; i < n; ++i) {
        dst[i] = src[i];
    }
}

__attribute__((noinline)) static void reverse(unsigned char* dst, const unsigned char* src, unsigned n) {
    for (unsigned i = n; i != 0; --i) {
        dst[i - 1] = src[i - 1];
    }
}

__attribute__((noinline)) static void strided(unsigned char* dst, const unsigned char* src, unsigned n, unsigned outStep, unsigned inStep) {
    unsigned input = 33;
    for (unsigned output = 19; output < 19 + n * outStep; output += outStep) {
        dst[output] = src[input];
        input += inStep;
    }
}

__attribute__((noinline)) static void indexed(unsigned char* dst, const unsigned char* src, const uint32_t* offset, unsigned n) {
    for (unsigned i = 0; i < n; ++i) {
        dst[i] = src[i + *offset];
    }
}

__attribute__((noinline)) static void indirect(unsigned char* dst, const unsigned char* const* src, unsigned n) {
    for (unsigned i = 0; i < n; ++i) {
        dst[i] = (*src)[i];
    }
}

__attribute__((noinline)) static void groups(unsigned char* dst, const unsigned char* src, unsigned n) {
    for (unsigned i = n & ~3u; i != 0; i -= 4) {
        dst[i - 4] = src[i - 4];
        dst[i - 3] = src[i - 3];
        dst[i - 2] = src[i - 2];
        dst[i - 1] = src[i - 1];
    }
}

__attribute__((noinline)) static void selected(unsigned char* dst, const unsigned char* src, unsigned n, unsigned mode) {
    for (unsigned i = 0; i < n; ++i) {
        unsigned selector = mode;
        asm volatile("" : "+r"(selector));
        switch (selector) {
            case 0: {
                dst[i] = src[i];
                break;
            }
            case 1: {
                dst[i] = src[i + 1];
                break;
            }
            case 2: {
                dst[i] = src[2 * i + 3];
                break;
            }
            case 3: {
                dst[i] = src[i] ^ 0x5a;
                break;
            }
            case 4: {
                dst[i] = src[i] + src[i + 1];
                break;
            }
            case 5: {
                dst[i] = src[n - 1 - i];
                break;
            }
            default: {
                dst[i] = src[i] + mode;
                break;
            }
        }
    }
}

static void padding(const unsigned char* dst, unsigned begin, unsigned end) {
    for (unsigned i = begin; i < end; ++i) {
        assert(dst[i] == 0xa5);
    }
}

int main() {
    unsigned char source[4096], destination[1024];
    for (unsigned i = 0; i < sizeof(source); ++i) {
        source[i] = (i * 37 + 11) % 251;
    }
    const unsigned counts[] = {0, 1, 7, 8, 15, 16, 17, 33, 97, 128};
    for (unsigned n : counts) {
        memset(destination, 0xa5, sizeof(destination));
        forward(destination, source + 13, n);
        assert(memcmp(destination, source + 13, n) == 0);
        padding(destination, n, sizeof(destination));
        memset(destination, 0xa5, sizeof(destination));
        reverse(destination, source + 19, n);
        assert(memcmp(destination, source + 19, n) == 0);
        padding(destination, n, sizeof(destination));
        memset(destination, 0xa5, sizeof(destination));
        groups(destination, source + 23, n);
        assert(memcmp(destination, source + 23, n & ~3u) == 0);
        padding(destination, n & ~3u, sizeof(destination));

        const unsigned steps[] = {1, 2, 3, 5};
        for (unsigned outStep : steps) {
            for (unsigned inStep : steps) {
                memset(destination, 0xa5, sizeof(destination));
                strided(destination, source, n, outStep, inStep);
                for (unsigned i = 0; i < sizeof(destination); ++i) {
                    bool written = i >= 19 && (i - 19) % outStep == 0 && (i - 19) / outStep < n;
                    assert(destination[i] == (written ? source[33 + (i - 19) / outStep * inStep] : 0xa5));
                }
            }
        }
        const uint32_t offsets[] = {0, 1, 127, 511};
        for (const uint32_t& offset : offsets) {
            memset(destination, 0xa5, sizeof(destination));
            indexed(destination, source, &offset, n);
            assert(memcmp(destination, source + offset, n) == 0);
            padding(destination, n, sizeof(destination));
        }
        const unsigned char* pointer = source + 57;
        memset(destination, 0xa5, sizeof(destination));
        indirect(destination, &pointer, n);
        assert(memcmp(destination, source + 57, n) == 0);
        padding(destination, n, sizeof(destination));
        for (unsigned mode = 0; mode != 8; ++mode) {
            memset(destination, 0xa5, sizeof(destination));
            unsigned selectedMode = mode == 7 ? 99 : mode;
            selected(destination, source, n, selectedMode);
            for (unsigned i = 0; i < n; ++i) {
                const unsigned char expected[] = {source[i], source[i + 1], source[2 * i + 3], static_cast<unsigned char>(source[i] ^ 0x5a), static_cast<unsigned char>(source[i] + source[i + 1]), source[n - 1 - i], static_cast<unsigned char>(source[i] + selectedMode)};
                assert(destination[i] == expected[selectedMode < 6 ? selectedMode : 6]);
            }
            padding(destination, n, sizeof(destination));
        }
    }

    struct State {
        uint32_t offset;
        unsigned char rest[128];
    } state;
    memset(&state, 0x55, sizeof(state));
    state.offset = 0;
    memset(source, 0, sizeof(source));
    source[0] = 1;
    source[2] = 2;
    auto* bytes = reinterpret_cast<unsigned char*>(&state);
    indexed(bytes, source, &state.offset, 64);
    assert(state.offset == 513 && bytes[0] == 1 && bytes[1] == 2);
    for (unsigned i = 2; i < sizeof(state); ++i) {
        assert(bytes[i] == (i < 64 ? 0 : 0x55));
    }
    puts("memory loops: ok");
}
