//@ host: false
//@ compile-flags: -g -D_GNU_SOURCE
//@ stdout: "random state bounds: ok\n"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main() {
    const size_t sizes[] = {32, 64, 128, 256};
    const uint32_t indices[] = {0xff00, 0x00ff, 0xffff};
    // Guest-owned random state follows musl's format. Damaged cursor bytes
    // must not let the host implementation read or write past that state.
    for (size_t size : sizes) {
        for (uint32_t bad : indices) {
            alignas(uint64_t) uint32_t guarded[514];
            for (uint32_t& word : guarded) {
                word = 0xa5a5a5a5;
            }
            char* state = reinterpret_cast<char*>(guarded + 2);
            char* previous = initstate(42, state, size);
            assert(previous != nullptr);
            uint32_t header;
            memcpy(&header, state, sizeof(header));
            uint32_t words = header >> 16;
            assert(words == size / 4 - 1);
            header = words << 16 | bad;
            memcpy(state, &header, sizeof(header));
            for (unsigned sample = 0; sample < 8; ++sample) {
                long value = random();
                assert(value >= 0 && value <= 0x7fffffff);
                memcpy(&header, state, sizeof(header));
                assert(header >> 16 == words);
                assert((header >> 8 & 255) < words && (header & 255) < words);
                for (size_t at = 0; at < sizeof(guarded) / sizeof(guarded[0]); ++at) {
                    if (at < 2 || at >= 2 + size / sizeof(uint32_t)) {
                        assert(guarded[at] == 0xa5a5a5a5);
                    }
                }
            }
            assert(setstate(previous) == state);
        }
    }
    puts("random state bounds: ok");
}
