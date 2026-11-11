//@ compile-flags: -g -fno-builtin
//@ stdout: "boolean addresses: ok\n"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

[[gnu::noinline]] static unsigned sample(const unsigned char* data, unsigned flags) {
    return data[flags == 0];
}

int main() {
    unsigned char data[257];
    for (unsigned i = 0; i < sizeof(data); ++i) {
        data[i] = (i * 37 + 11) % 251;
    }
    volatile unsigned flags[] = {0, 1, 2, 127, 128, 0x7fffffff, 0x80000000, UINT32_MAX};
    for (unsigned offset = 0; offset != 256; ++offset) {
        for (unsigned i = 0; i < sizeof(flags) / sizeof(flags[0]); ++i) {
            unsigned value = flags[i];
            assert(sample(data + offset, value) == data[offset + (value == 0)]);
        }
    }
    puts("boolean addresses: ok");
}
