//@ compile-flags: -g -ffp-contract=off
//@ stdout: "bounded floats: ok\n"

#include <assert.h>
#include <math.h>
#include <stdio.h>

template <typename Float>
[[gnu::noinline]] static int evaluated(Float x, unsigned operation) {
    if (x < Float(-16) || x > Float(16) || x != x) {
        return 1337;
    }
    switch (operation) {
        case 0: {
            return int(x);
        }
        case 1: {
            return int(x + Float(1));
        }
        case 2: {
            return int(Float(5) - x);
        }
        case 3: {
            return int(x * Float(0.5));
        }
        case 4: {
            return int(x / Float(2));
        }
        case 5: {
            return int(-x);
        }
        case 6: {
            return int(Float(__builtin_fabs(x)));
        }
        default: {
            return int(x >= 0 ? x : x + Float(1));
        }
    }
}

template <typename Float>
[[gnu::noinline]] static int equal(Float x) {
    if (x == Float(2.5)) {
        return int(x);
    }
    if (Float(0) == x) {
        return int(x);
    }
    return 1337;
}

int main() {
    for (int i = -128; i <= 128; ++i) {
        int expected[] = {i / 4, (i + 4) / 4, (20 - i) / 4, i / 8, i / 8, -i / 4, (i < 0 ? -i : i) / 4, i >= 0 ? i / 4 : (i + 4) / 4};
        for (unsigned operation = 0; operation < 8; ++operation) {
            int value = i >= -64 && i <= 64 ? expected[operation] : 1337;
            assert(evaluated(float(i) / 4, operation) == value);
            assert(evaluated(double(i) / 4, operation) == value);
        }
        int value = i == 10 ? 2 : i == 0 ? 0 : 1337;
        assert(equal(float(i) / 4) == value && equal(double(i) / 4) == value);
    }
    const double exceptional[] = {INFINITY, -INFINITY, NAN};
    for (double value : exceptional) {
        for (unsigned operation = 0; operation < 8; ++operation) {
            assert(evaluated(value, operation) == 1337 && evaluated(float(value), operation) == 1337);
        }
        assert(equal(value) == 1337 && equal(float(value)) == 1337);
    }
    const double tiny[] = {-0.0, 0.0, 0x1p-1074, -0x1p-1074};
    for (double value : tiny) {
        assert(evaluated(value, 0) == 0 && evaluated(value, 6) == 0);
    }
    puts("bounded floats: ok");
}
