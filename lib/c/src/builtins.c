#include <stdint.h>

/*
 * Compiler runtime: wasm32 has no 128-bit multiply, so LLVM calls __multi3
 * for __int128 products and for 64-bit overflow checks. Only 64-bit
 * arithmetic is used here; an __int128 operator would call back into us.
 */
typedef union {
    __int128 all;
    struct {
        uint64_t low;
        uint64_t high;
    } parts;
} Words;

static Words multiplyWide(uint64_t a, uint64_t b) {
    const uint64_t lowerMask = 0xffffffffULL;
    Words r;
    uint64_t t;

    r.parts.low = (a & lowerMask) * (b & lowerMask);
    t = r.parts.low >> 32;
    r.parts.low &= lowerMask;
    t += (a >> 32) * (b & lowerMask);
    r.parts.low += (t & lowerMask) << 32;
    r.parts.high = t >> 32;
    t = r.parts.low >> 32;
    r.parts.low &= lowerMask;
    t += (b >> 32) * (a & lowerMask);
    r.parts.low += (t & lowerMask) << 32;
    r.parts.high += t >> 32;
    r.parts.high += (a >> 32) * (b >> 32);

    return r;
}

__int128 __multi3(__int128 a, __int128 b) {
    Words x;
    Words y;
    Words r;

    x.all = a;
    y.all = b;
    r = multiplyWide(x.parts.low, y.parts.low);
    r.parts.high += x.parts.high * y.parts.low + x.parts.low * y.parts.high;

    return r.all;
}
