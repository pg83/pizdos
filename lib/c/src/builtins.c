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

/*
 * 128-bit division, which libc++'s conversions use: a quotient and remainder
 * of 64-bit halves, by native division when both fit in 64 bits and by
 * shift and subtract from the divisor's highest bit otherwise.
 */
static int leadingZeros(Words x) {
    if (x.parts.high) {
        return __builtin_clzll(x.parts.high);
    }

    return x.parts.low ? 64 + __builtin_clzll(x.parts.low) : 128;
}

static Words shiftLeft(Words x, int n) {
    Words r;

    if (n == 0) {
        return x;
    }

    if (n >= 64) {
        r.parts.high = x.parts.low << (n - 64);
        r.parts.low = 0;
    } else {
        r.parts.high = x.parts.high << n | x.parts.low >> (64 - n);
        r.parts.low = x.parts.low << n;
    }

    return r;
}

static Words divideUnsigned(Words n, Words d, Words* remainder) {
    Words q;
    q.parts.high = 0;
    q.parts.low = 0;

    if (!n.parts.high && !d.parts.high) {
        q.parts.low = n.parts.low / d.parts.low;
        remainder->parts.high = 0;
        remainder->parts.low = n.parts.low % d.parts.low;

        return q;
    }

    int shift = leadingZeros(d) - leadingZeros(n);

    if (shift < 0) {
        *remainder = n;

        return q;
    }

    Words s = shiftLeft(d, shift);

    for (int i = 0; i <= shift; i++) {
        q.parts.high = q.parts.high << 1 | q.parts.low >> 63;
        q.parts.low <<= 1;

        if (n.parts.high > s.parts.high || (n.parts.high == s.parts.high && n.parts.low >= s.parts.low)) {
            uint64_t borrow = n.parts.low < s.parts.low;
            n.parts.low -= s.parts.low;
            n.parts.high -= s.parts.high + borrow;
            q.parts.low |= 1;
        }

        s.parts.low = s.parts.low >> 1 | s.parts.high << 63;
        s.parts.high >>= 1;
    }

    *remainder = n;

    return q;
}

static Words negate(Words x) {
    x.parts.low = ~x.parts.low + 1;
    x.parts.high = ~x.parts.high + (x.parts.low == 0);

    return x;
}

static Words magnitude(Words x) {
    return (int64_t)x.parts.high < 0 ? negate(x) : x;
}

unsigned __int128 __udivti3(unsigned __int128 a, unsigned __int128 b) {
    Words x;
    Words y;
    Words r;

    x.all = a;
    y.all = b;

    return divideUnsigned(x, y, &r).all;
}

unsigned __int128 __umodti3(unsigned __int128 a, unsigned __int128 b) {
    Words x;
    Words y;
    Words r;

    x.all = a;
    y.all = b;
    divideUnsigned(x, y, &r);

    return r.all;
}

__int128 __divti3(__int128 a, __int128 b) {
    Words x;
    Words y;
    Words r;

    x.all = a;
    y.all = b;
    Words q = divideUnsigned(magnitude(x), magnitude(y), &r);

    return ((int64_t)(x.parts.high ^ y.parts.high) < 0 ? negate(q) : q).all;
}

__int128 __modti3(__int128 a, __int128 b) {
    Words x;
    Words y;
    Words r;

    x.all = a;
    y.all = b;
    divideUnsigned(magnitude(x), magnitude(y), &r);

    return ((int64_t)x.parts.high < 0 ? negate(r) : r).all;
}
