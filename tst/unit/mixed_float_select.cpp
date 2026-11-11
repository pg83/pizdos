//@ compile-flags: -g -ffp-contract=off
//@ stdout: "mixed float select: ok\n"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>

static volatile float inputs[] = {-1, -0.0f, 0.0f, 1, NAN, INFINITY};
static volatile uint64_t wideBits[] = {0, 0x8000000000000000ULL, 0x7fefffffffffffffULL, 0x7ff0000000000000ULL, 0xfff0000000000000ULL, 0x7ff8000000001234ULL};
static volatile uint32_t narrowBits[] = {0, 0x80000000U, 0x7f7fffffU, 0x7f800000U, 0xff800000U, 0x7fc01234U};

template <int Relation, typename Input, typename Output>
[[gnu::noinline]] static Output selected(Input a, Input b, Output yes, Output no) {
    if constexpr (Relation == 0) {
        return a == b ? yes : no;
    }
    if constexpr (Relation == 1) {
        return a != b ? yes : no;
    }
    if constexpr (Relation == 2) {
        return a < b ? yes : no;
    }
    if constexpr (Relation == 3) {
        return a > b ? yes : no;
    }
    if constexpr (Relation == 4) {
        return a <= b ? yes : no;
    }
    if constexpr (Relation == 5) {
        return a >= b ? yes : no;
    }
}

template <int Relation>
static void check(unsigned i, unsigned j, bool truth) {
    float a = inputs[i], b = inputs[j];
    for (unsigned k = 0; k < 6; ++k) {
        uint64_t x = wideBits[k], y = wideBits[(k + 1) % 6];
        double yes = __builtin_bit_cast(double, x), no = __builtin_bit_cast(double, y);
        assert(__builtin_bit_cast(uint64_t, selected<Relation>(a, b, yes, no)) == (truth ? x : y));
        assert(__builtin_bit_cast(uint64_t, selected<Relation>(double(a), double(b), yes, no)) == (truth ? x : y));
        uint32_t nx = narrowBits[k], ny = narrowBits[(k + 1) % 6];
        float narrowYes = __builtin_bit_cast(float, nx), narrowNo = __builtin_bit_cast(float, ny);
        assert(__builtin_bit_cast(uint32_t, selected<Relation>(a, b, narrowYes, narrowNo)) == (truth ? nx : ny));
        assert(__builtin_bit_cast(uint32_t, selected<Relation>(double(a), double(b), narrowYes, narrowNo)) == (truth ? nx : ny));
    }
}

int main() {
    // Integer ranks independently determine the ordered outcomes. Both
    // signs of zero compare equal, and index four is unordered.
    const unsigned ranks[] = {0, 1, 1, 2, 0, 3};
    for (unsigned i = 0; i < 6; ++i) {
        for (unsigned j = 0; j < 6; ++j) {
            bool ordered = i != 4 && j != 4;
            check<0>(i, j, ordered && ranks[i] == ranks[j]);
            check<1>(i, j, !ordered || ranks[i] != ranks[j]);
            check<2>(i, j, ordered && ranks[i] < ranks[j]);
            check<3>(i, j, ordered && ranks[i] > ranks[j]);
            check<4>(i, j, ordered && ranks[i] <= ranks[j]);
            check<5>(i, j, ordered && ranks[i] >= ranks[j]);
        }
    }
    puts("mixed float select: ok");
}
