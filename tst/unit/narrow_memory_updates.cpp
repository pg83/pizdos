//@ compile-flags: -g -fno-builtin
//@ stdout: "narrow memory updates: ok\n"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

[[gnu::noinline]] static void update(volatile uint16_t* words, unsigned operation) {
    switch (operation) {
        case 0: {
            words[1] += 0x1234;
            break;
        }
        case 1: {
            words[1] -= 0x3456;
            break;
        }
        case 2: {
            words[1] ^= 0x5678;
            break;
        }
        case 3: {
            words[1] |= 0x2345;
            break;
        }
        case 4: {
            words[1] &= 0x6789;
            break;
        }
        case 5: {
            words[1] += 127;
            break;
        }
        default: {
            words[1] -= 128;
            break;
        }
    }
}

int main() {
    const uint16_t values[] = {0, 1, 127, 128, 255, 256, 32767, 32768, 65534, 65535};
    for (uint32_t value : values) {
        const uint16_t expected[] = {uint16_t(value + 0x1234), uint16_t(value - 0x3456), uint16_t(value ^ 0x5678), uint16_t(value | 0x2345), uint16_t(value & 0x6789), uint16_t(value + 127), uint16_t(value - 128)};
        for (unsigned operation = 0; operation < 7; ++operation) {
            volatile uint16_t words[] = {0xa55a, uint16_t(value), 0x5aa5};
            update(words, operation);
            assert(words[0] == 0xa55a && words[1] == expected[operation] && words[2] == 0x5aa5);
        }
    }
    puts("narrow memory updates: ok");
}
