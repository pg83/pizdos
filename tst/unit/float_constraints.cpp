//@ compile-flags: -g -ffp-contract=off
//@ stdout: "float constraints: ok\n"

#include <assert.h>
#include <math.h>
#include <stdio.h>

static volatile unsigned visits;

template <typename Float>
[[gnu::always_inline]] static inline Float opaque(Float value) {
    // Preserve the expression's order in LLVM; Wasm needs no instruction here.
    __asm__("" : "+r"(value));
    return value;
}

template <typename Float, int Relation, bool Left, bool Truth>
[[gnu::noinline]] static int constrained(Float x) {
    if (!(x >= Float(-16) && x <= Float(16))) {
        return 1337;
    }
    Float constant = opaque(Float(2.5));
    Float a = Left ? constant : x, b = Left ? x : constant;
    bool condition;
    if constexpr (Relation == 0) condition = a == b;
    if constexpr (Relation == 1) condition = a != b;
    if constexpr (Relation == 2) condition = a < b;
    if constexpr (Relation == 3) condition = a <= b;
    if constexpr (Relation == 4) condition = a > b;
    if constexpr (Relation == 5) condition = a >= b;
    if (condition == Truth) {
        visits = visits + 1;
        return int(x);
    }
    return 1337;
}

template <typename Float>
[[gnu::noinline]] static int arithmetic(Float x, Float y, unsigned operation) {
    if (!(x >= Float(-16) && x <= Float(16)) || y != opaque(Float(3))) {
        return 1337;
    }
    visits = visits + 1;
    switch (operation) {
        case 0: return int(x - y);
        case 1: return int(x / y);
        case 2: return int(x * y);
        default: return int(x + y);
    }
}

template <typename Float, int Relation>
static unsigned comparisons(Float x, bool condition, bool reversed) {
    int value = int(x);
    assert((constrained<Float, Relation, false, true>(x) == (condition ? value : 1337)));
    assert((constrained<Float, Relation, false, false>(x) == (!condition ? value : 1337)));
    assert((constrained<Float, Relation, true, true>(x) == (reversed ? value : 1337)));
    assert((constrained<Float, Relation, true, false>(x) == (!reversed ? value : 1337)));
    return 2;
}

template <typename Float>
static unsigned check(int quarter) {
    Float x = Float(quarter) / 4;
    unsigned count = comparisons<Float, 0>(x, quarter == 10, quarter == 10);
    count += comparisons<Float, 1>(x, quarter != 10, quarter != 10);
    count += comparisons<Float, 2>(x, quarter < 10, quarter > 10);
    count += comparisons<Float, 3>(x, quarter <= 10, quarter >= 10);
    count += comparisons<Float, 4>(x, quarter > 10, quarter < 10);
    count += comparisons<Float, 5>(x, quarter >= 10, quarter <= 10);
    int expected[] = {(quarter - 12) / 4, quarter / 12, quarter * 3 / 4, (quarter + 12) / 4};
    for (unsigned operation = 0; operation < 4; ++operation) {
        assert(arithmetic(x, Float(3), operation) == expected[operation]);
        assert(arithmetic(x, Float(2), operation) == 1337);
        ++count;
    }
    return count;
}

int main() {
    unsigned expected = 0;
    for (int quarter = -64; quarter <= 64; ++quarter) {
        expected += check<float>(quarter) + check<double>(quarter);
    }
    assert(visits == expected);
    const double invalid[] = {NAN, INFINITY, -INFINITY, -17, 17};
    for (double x : invalid) {
        assert((constrained<double, 2, true, true>(x) == 1337));
        assert((constrained<float, 3, true, false>(float(x)) == 1337));
        for (unsigned operation = 0; operation < 4; ++operation) {
            assert(arithmetic(x, 3.0, operation) == 1337);
        }
    }
    assert(visits == expected);
    puts("float constraints: ok");
}
