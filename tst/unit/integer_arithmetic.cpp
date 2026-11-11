//@ stdout: "integer arithmetic: ok\n"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static volatile uint64_t input[] = {0, 1, 3, 19, 0xffffffffULL, 0x8000000000000000ULL, 0xffffffffffffffffULL};

static void check(bool condition) {
    if (!condition) {
        abort();
    }
}

__attribute__((noinline)) static uint64_t arithmetic(uint64_t a, uint64_t b) {
    return ((a + b) ^ (a - b)) + a * b;
}

__attribute__((noinline)) static uint64_t rotate(uint64_t value, unsigned shift) {
    shift &= 63;
    return (value << shift) | (value >> ((64 - shift) & 63));
}

__attribute__((noinline)) static int64_t divide(int64_t a, int64_t b) {
    return a / b;
}

__attribute__((noinline)) static int64_t remainder(int64_t a, int64_t b) {
    return a % b;
}

int main() {
    auto operation = arithmetic;
    for (unsigned i = 0; i < sizeof(input) / sizeof(input[0]); ++i) {
        uint64_t value = input[i];
        check(operation(value, 0) == 0);
        check(operation(value, 1) == (((value + 1) ^ (value - 1)) + value));
        check(rotate(rotate(value, 17), 47) == value);
        check((value & ~value) == 0);
        check((value | ~value) == UINT64_MAX);
        check((uint32_t)value == (value & UINT32_MAX));
        check(__builtin_popcountll(value) + __builtin_popcountll(~value) == 64);
        if (value != 0) {
            check((value >> (63 - __builtin_clzll(value))) == 1);
            check((value & (uint64_t(1) << __builtin_ctzll(value))) != 0);
        }
    }
    int64_t value = int64_t(input[3]);
    check(divide(value, 3) == 6);
    check(divide(-value, 3) == -6);
    check(divide(value, -3) == -6);
    check(divide(-value, -3) == 6);
    check(remainder(value, 3) == 1);
    check(remainder(-value, 3) == -1);
    check(remainder(value, -3) == 1);
    check(remainder(-value, -3) == -1);
    check(divide(INT64_MIN, 1) == INT64_MIN);
    check(remainder(INT64_MIN, 3) == -2);
    puts("integer arithmetic: ok");
    return 0;
}
