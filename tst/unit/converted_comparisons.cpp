//@ compile-flags: -g
//@ stdout: "converted comparisons: ok\n"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

[[gnu::always_inline]] static inline double converted(uint32_t value) {
    double result = value;
    __asm__("" : "+r"(result));
    return result;
}

[[gnu::noinline]] static unsigned relations(uint32_t a, uint32_t b) {
    double x = converted(a), y = converted(b);
    return unsigned(x == y) | (unsigned(x != y) << 1) | (unsigned(x < y) << 2) | (unsigned(x <= y) << 3) | (unsigned(x > y) << 4) | (unsigned(x >= y) << 5);
}

[[gnu::noinline]] static unsigned selected(uint32_t a, uint32_t b, unsigned selector) {
    double x = selector ? converted(a) : converted(b);
    double y = selector ? converted(b) : converted(a);
    return unsigned(x < y) | (unsigned(x == y) << 1) | (unsigned(x > y) << 2);
}

int main() {
    const uint32_t values[] = {0, 1, 2, 16777215, 16777216, 16777217, 2147483647, 2147483648u, 4294967294u, 4294967295u};
    for (uint32_t a : values) {
        for (uint32_t b : values) {
            unsigned expected = a == b ? 41 : a < b ? 14 : 50;
            assert(relations(a, b) == expected);
            assert(selected(a, b, 1) == (a == b ? 2u : a < b ? 1u : 4u));
            assert(selected(a, b, 0) == (a == b ? 2u : a > b ? 1u : 4u));
        }
    }
    puts("converted comparisons: ok");
}
