//@ stdout: "floating arithmetic: ok\n"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static volatile double numbers[] = {3.75, -3.75, 0.5, -0.0, 9223372036854774784.0, 18446744073709547520.0};

static void check(bool condition) {
    if (!condition) {
        abort();
    }
}

__attribute__((noinline)) static double calculate(double a, double b, unsigned operation) {
    switch (operation) {
        case 0: {
            return a + b;
        }
        case 1: {
            return a - b;
        }
        case 2: {
            return a * b;
        }
        case 3: {
            return a / b;
        }
        case 4: {
            return __builtin_sqrt(a);
        }
        case 5: {
            return __builtin_floor(a);
        }
        case 6: {
            return __builtin_ceil(a);
        }
        case 7: {
            return __builtin_trunc(a);
        }
        case 8: {
            return __builtin_nearbyint(a);
        }
        default: {
            return -a;
        }
    }
}

__attribute__((noinline)) static float narrow(float a, float b) {
    return a * b + a / b;
}

__attribute__((noinline)) static int64_t signedInteger(double value) {
    return int64_t(value);
}

__attribute__((noinline)) static uint64_t unsignedInteger(double value) {
    return uint64_t(value);
}

__attribute__((noinline)) static double floating(uint64_t value) {
    return double(value);
}

int main() {
    double positive = numbers[0];
    double negative = numbers[1];
    double half = numbers[2];
    check(calculate(positive, half, 0) == 4.25);
    check(calculate(positive, half, 1) == 3.25);
    check(calculate(positive, half, 2) == 1.875);
    check(calculate(positive, half, 3) == 7.5);
    check(calculate(half * half, 0, 4) == half);
    check(calculate(positive, 0, 5) == 3);
    check(calculate(negative, 0, 5) == -4);
    check(calculate(positive, 0, 6) == 4);
    check(calculate(negative, 0, 6) == -3);
    check(calculate(positive, 0, 7) == 3);
    check(calculate(negative, 0, 7) == -3);
    check(calculate(positive, 0, 8) == 4);
    check(calculate(negative, 0, 8) == -4);
    check(calculate(positive, 0, 9) == negative);
    check(narrow(float(positive), float(half)) == 9.375f);
    check(signedInteger(positive) == 3);
    check(signedInteger(negative) == -3);
    check(signedInteger(numbers[4]) == INT64_MAX - 1023);
    check(unsignedInteger(numbers[5]) == UINT64_MAX - 4095);
    check(floating(UINT64_MAX - 4095) == numbers[5]);
    check(__builtin_bit_cast(uint64_t, double(numbers[3])) == 0x8000000000000000ULL);
    double nan = __builtin_nan("");
    check(!(nan == positive));
    check(nan != positive);
    check(!(nan < positive));
    check(!(nan <= positive));
    check(!(nan > positive));
    check(!(nan >= positive));
    puts("floating arithmetic: ok");
    return 0;
}
