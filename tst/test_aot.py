#!/usr/bin/env python3
"""Code from aot runs right: ops.wasm compiled by aot, mapped and called
through tst/aot/harness.py, checked against wasm's semantics written out in
Python.

    test_aot.py <ops.wasm> <ops.aot>
"""
import ctypes
import math
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "aot"))

from harness import (  # noqa: E402
    CONVERSION,
    DIVISION_BY_ZERO,
    GLOBALS,
    Instance,
    MEMORY_SIZE,
    OUT_OF_BOUNDS,
    OVERFLOW,
    PAGE,
    SIGNATURE,
    STACK_LIMIT,
    TABLE,
    TABLE_SIZE,
    Trap,
    UNREACHABLE,
)

def s32(x):
    x &= 0xFFFFFFFF

    return x - (1 << 32) if x >> 31 else x


def s64(x):
    x &= 0xFFFFFFFFFFFFFFFF

    return x - (1 << 64) if x >> 63 else x


def f32(x):
    return ctypes.c_float(x).value


def bits64(x):
    return struct.unpack("<Q", struct.pack("<d", x))[0]


def same(actual, expected):
    if isinstance(expected, float):
        return isinstance(actual, float) and (math.isnan(actual) and math.isnan(expected) or bits64(actual) == bits64(expected))

    return actual == expected


def divide(a, b, wrap, minimum, signed, remainder):
    if b == 0:
        return Trap(DIVISION_BY_ZERO)

    if signed and a == minimum and b == -1 and not remainder:
        return Trap(OVERFLOW)

    quotient = abs(a) // abs(b)

    if (a < 0) != (b < 0):
        quotient = -quotient

    return wrap(a - b * quotient if remainder else quotient)


def fdiv(a, b):
    try:
        return a / b
    except ZeroDivisionError:
        if math.isnan(a) or a == 0:
            return math.nan

        return math.copysign(math.inf, a) * math.copysign(1.0, b)


def rounding(function):
    def apply(x):
        if math.isnan(x) or math.isinf(x) or x == 0:
            return x

        return math.copysign(float(function(x)), x)

    return apply


def fmin(a, b):
    if math.isnan(a) or math.isnan(b):
        return math.nan

    if a == b == 0:
        return a if math.copysign(1, a) < 0 else b

    return min(a, b)


def fmax(a, b):
    if math.isnan(a) or math.isnan(b):
        return math.nan

    if a == b == 0:
        return a if math.copysign(1, a) > 0 else b

    return max(a, b)


def sqrt(x):
    if math.isnan(x) or x < 0:
        return math.nan

    return math.sqrt(x)


def truncate(x, low, high, saturate):
    """wasm's float to integer truncation into [low, high]."""
    if math.isnan(x):
        return 0 if saturate else Trap(CONVERSION)

    if math.isinf(x) or not low <= math.trunc(x) <= high:
        if saturate:
            return low if x < 0 else high

        return Trap(OVERFLOW)

    return math.trunc(x)


def f32_of_int(n):
    """Round an integer to f32 directly, not through f64."""
    magnitude = abs(n)
    shift = max(magnitude.bit_length() - 24, 0)
    quotient, rest = divmod(magnitude, 1 << shift)
    half = 1 << shift >> 1

    if shift and (rest > half or rest == half and quotient & 1):
        quotient += 1

    return math.copysign(float(quotient << shift), n) if n else 0.0


def compare_bits(a, b, unsigned):
    if unsigned is None:
        relations = [a == b, a != b, a < b, a > b, a <= b, a >= b]
    else:
        x, y = a & unsigned, b & unsigned
        relations = [a == b, a != b, a < b, x < y, a > b, x > y, a <= b, x <= y, a >= b, x >= y, a == 0]

    return sum(int(bool(r)) << i for i, r in enumerate(relations))


I32 = [0, 1, -1, 2, -2, 7, -7, 31, 32, 33, 0x55555555, 0x12345678, -0x12345678, 0x7FFFFFFF, -0x80000000]
I64 = [0, 1, -1, 3, -3, 63, 64, 65, (1 << 32) - 1, 1 << 32, 0x123456789ABCDEF0, -0x123456789ABCDEF, (1 << 63) - 1, -(1 << 63)]
F64 = [
    0.0, -0.0, 0.5, -0.5, 1.0, -1.0, 1.5, 2.5, -2.5, 3.7, -3.7, 1e10, -1e10, 1e300, -1e300, 5e-324,
    math.inf, -math.inf, math.nan, 2.0**31, -2.0**31, 2.0**31 - 0.5, -2.0**31 - 1, 2.0**32, 4294967295.5,
    2.0**63, -2.0**63, 2.0**64, 9007199254740993.0,
]
F32 = [f32(x) for x in F64]


def cases():
    """(function, arguments, expected) for every pure operator."""
    mask32, mask64 = 0xFFFFFFFF, 0xFFFFFFFFFFFFFFFF

    for a in I32:
        for b in I32:
            yield "i32_add", (a, b), s32(a + b)
            yield "i32_sub", (a, b), s32(a - b)
            yield "i32_mul", (a, b), s32(a * b)
            yield "i32_div_s", (a, b), divide(a, b, s32, -(1 << 31), True, False)
            yield "i32_rem_s", (a, b), divide(a, b, s32, -(1 << 31), True, True)
            yield "i32_div_u", (a, b), divide(a & mask32, b & mask32, s32, 0, False, False)
            yield "i32_rem_u", (a, b), divide(a & mask32, b & mask32, s32, 0, False, True)
            yield "i32_and", (a, b), a & b
            yield "i32_or", (a, b), a | b
            yield "i32_xor", (a, b), a ^ b
            yield "i32_shl", (a, b), s32(a << (b & 31))
            yield "i32_shr_s", (a, b), a >> (b & 31)
            yield "i32_shr_u", (a, b), s32((a & mask32) >> (b & 31))
            yield "i32_rotl", (a, b), s32((a & mask32) << (b & 31) | (a & mask32) >> (32 - (b & 31)))
            yield "i32_rotr", (a, b), s32((a & mask32) >> (b & 31) | (a & mask32) << (32 - (b & 31)))
            yield "i32_compare", (a, b), compare_bits(a, b, mask32)
            yield "max_s", (a, b), max(a, b)

        x = a & mask32
        yield "i32_clz", (a,), 32 - x.bit_length()
        yield "i32_ctz", (a,), (x & -x).bit_length() - 1 if x else 32
        yield "i32_popcnt", (a,), bin(x).count("1")
        yield "i32_extend8_s", (a,), s32((a & 0xFF) - (0x100 if a & 0x80 else 0))
        yield "i32_extend16_s", (a,), s32((a & 0xFFFF) - (0x10000 if a & 0x8000 else 0))
        yield "i64_extend_i32_s", (a,), a
        yield "i64_extend_i32_u", (a,), x
        yield "f32_convert_i32_s", (a,), f32_of_int(a)
        yield "f32_convert_i32_u", (a,), f32_of_int(x)
        yield "f64_convert_i32_s", (a,), float(a)
        yield "f64_convert_i32_u", (a,), float(x)
        yield "f32_reinterpret_i32", (a,), struct.unpack("<f", struct.pack("<i", a))[0]

    for a in I64:
        for b in I64:
            yield "i64_add", (a, b), s64(a + b)
            yield "i64_sub", (a, b), s64(a - b)
            yield "i64_mul", (a, b), s64(a * b)
            yield "i64_div_s", (a, b), divide(a, b, s64, -(1 << 63), True, False)
            yield "i64_rem_s", (a, b), divide(a, b, s64, -(1 << 63), True, True)
            yield "i64_div_u", (a, b), divide(a & mask64, b & mask64, s64, 0, False, False)
            yield "i64_rem_u", (a, b), divide(a & mask64, b & mask64, s64, 0, False, True)
            yield "i64_and", (a, b), a & b
            yield "i64_or", (a, b), a | b
            yield "i64_xor", (a, b), a ^ b
            yield "i64_shl", (a, b), s64(a << (b & 63))
            yield "i64_shr_s", (a, b), a >> (b & 63)
            yield "i64_shr_u", (a, b), s64((a & mask64) >> (b & 63))
            yield "i64_rotl", (a, b), s64((a & mask64) << (b & 63) | (a & mask64) >> (64 - (b & 63)))
            yield "i64_rotr", (a, b), s64((a & mask64) >> (b & 63) | (a & mask64) << (64 - (b & 63)))
            yield "i64_compare", (a, b), compare_bits(a, b, mask64)

        x = a & mask64
        yield "i64_clz", (a,), 64 - x.bit_length()
        yield "i64_ctz", (a,), (x & -x).bit_length() - 1 if x else 64
        yield "i64_popcnt", (a,), bin(x).count("1")
        yield "i64_extend8_s", (a,), (a & 0xFF) - (0x100 if a & 0x80 else 0)
        yield "i64_extend16_s", (a,), (a & 0xFFFF) - (0x10000 if a & 0x8000 else 0)
        yield "i64_extend32_s", (a,), s32(a)
        yield "i32_wrap_i64", (a,), s32(a)
        yield "f32_convert_i64_s", (a,), f32_of_int(a)
        yield "f32_convert_i64_u", (a,), f32_of_int(x)
        yield "f64_convert_i64_s", (a,), float(a)
        yield "f64_convert_i64_u", (a,), float(x)
        yield "f64_reinterpret_i64", (a,), struct.unpack("<d", struct.pack("<q", a))[0]

    for width, values, wrap in [("f32", F32, f32), ("f64", F64, float)]:
        for a in values:
            for b in values:
                yield f"{width}_add", (a, b), wrap(a + b)
                yield f"{width}_sub", (a, b), wrap(a - b)
                yield f"{width}_mul", (a, b), wrap(a * b)
                yield f"{width}_div", (a, b), wrap(fdiv(a, b))
                yield f"{width}_min", (a, b), fmin(a, b)
                yield f"{width}_max", (a, b), fmax(a, b)
                yield f"{width}_copysign", (a, b), math.copysign(a, b)
                yield f"{width}_compare", (a, b), compare_bits(a, b, None)

            yield f"{width}_abs", (a,), abs(a)
            yield f"{width}_neg", (a,), -a
            yield f"{width}_sqrt", (a,), wrap(sqrt(a))
            yield f"{width}_ceil", (a,), rounding(math.ceil)(a)
            yield f"{width}_floor", (a,), rounding(math.floor)(a)
            yield f"{width}_trunc", (a,), rounding(math.trunc)(a)
            yield f"{width}_nearest", (a,), rounding(round)(a)

            for saturate in ("", "sat_"):
                yield f"i32_trunc_{saturate}{width}_s", (a,), truncate(a, -(1 << 31), (1 << 31) - 1, saturate)
                yield f"i64_trunc_{saturate}{width}_s", (a,), truncate(a, -(1 << 63), (1 << 63) - 1, saturate)
                unsigned32 = truncate(a, 0, (1 << 32) - 1, saturate)
                unsigned64 = truncate(a, 0, (1 << 64) - 1, saturate)
                yield f"i32_trunc_{saturate}{width}_u", (a,), unsigned32 if isinstance(unsigned32, Trap) else s32(unsigned32)
                yield f"i64_trunc_{saturate}{width}_u", (a,), unsigned64 if isinstance(unsigned64, Trap) else s64(unsigned64)

    for a in F64:
        yield "f32_demote_f64", (a,), f32(a)
        yield "i64_reinterpret_f64", (a,), s64(bits64(a)) if not math.isnan(a) else None

    for a in F32:
        yield "f64_promote_f32", (a,), a
        yield "i32_reinterpret_f32", (a,), s32(struct.unpack("<I", struct.pack("<f", a))[0]) if not math.isnan(a) else None


def check(instance, name, args, expected):
    if expected is None:
        return

    if isinstance(expected, Trap):
        actual = instance.run(name, *args)
        assert isinstance(actual, Trap) and actual.code == expected.code, f"{name}{args}: {actual}, expected {expected}"
        return

    actual = instance.call(name, *args)
    assert same(actual, expected), f"{name}{args}: {actual!r}, expected {expected!r}"


def test_memory(instance):
    size = instance.vmctx[MEMORY_SIZE]
    assert instance.call("memory_size") == size // PAGE
    message = instance.call("message_address")
    assert bytes(instance.memory[message:message + 13]) == b"data segment\0"

    address = 1000
    instance.call("poke64", address, 0x1122334455667788)
    assert bytes(instance.memory[address:address + 8]) == bytes.fromhex("8877665544332211")
    assert instance.call("peek8u", address) == 0x88
    assert instance.call("peek8s", address) == 0x88 - 0x100
    assert instance.call("peek16u", address + 1) == 0x6677
    assert instance.call("peek16s", address + 6) == 0x1122
    assert instance.call("peek32u", address + 4) == 0x11223344
    assert instance.call("peek32s", address) == s32(0x55667788)
    assert instance.call("peek64", address) == 0x1122334455667788
    instance.call("poke8", address, -1)
    instance.call("poke16", address + 2, 0x10000 + 0x4242)
    instance.call("poke32", address + 4, 0x7FFFFFFF)
    assert bytes(instance.memory[address:address + 8]) == bytes.fromhex("ff774242ffffff7f")
    instance.call("pokef32", address, 1.5)
    assert instance.call("peekf32", address) == 1.5
    instance.call("pokef64", address, -2.25)
    assert instance.call("peekf64", address) == -2.25

    instance.memory[2000:2010] = bytes(range(10))
    instance.call("copy", 2002, 2000, 6)
    assert bytes(instance.memory[2000:2010]) == bytes([0, 1, 0, 1, 2, 3, 4, 5, 8, 9])
    instance.call("fill", 2001, 0x1AB, 3)
    assert bytes(instance.memory[2000:2005]) == bytes([0, 0xAB, 0xAB, 0xAB, 2])

    assert instance.call("peek8u", size - 1) == 0
    assert instance.call("peek64", size - 8) == 0
    assert instance.run("peek8u", size).code == OUT_OF_BOUNDS
    assert instance.run("peek32u", size - 3).code == OUT_OF_BOUNDS
    assert instance.run("poke64", size - 7, 0).code == OUT_OF_BOUNDS
    assert instance.run("peek8u", -1).code == OUT_OF_BOUNDS
    assert instance.run("copy", size - 2, 0, 3).code == OUT_OF_BOUNDS

    pages = size // PAGE
    assert instance.call("memory_grow", 2) == pages
    assert instance.call("memory_size") == pages + 2
    assert instance.call("peek8u", size) == 0
    assert instance.call("memory_grow", 1 << 20) == -1


def test_control(instance, kernel_calls):
    fibonacci = [0, 1]

    while len(fibonacci) < 25:
        fibonacci.append(fibonacci[-1] + fibonacci[-2])

    for n in range(25):
        assert instance.call("fib", n) == fibonacci[n]

    for n in [1, 2, 3, 6, 7, 27, 97, 871, 63728127]:
        steps, x = 0, n

        while x != 1:
            x = 3 * x + 1 if x & 1 else x // 2
            steps += 1

        assert instance.call("collatz", n) == steps

    table = {0: 17, 1: 4, 2: 99, 3: -5, 4: 1000, 5: 3, 7: 70, 8: 81}

    for x in range(-3, 13):
        assert instance.call("classify", x) == table.get(x, -1)

    for n in [0, 1, 7, -3, 1 << 26]:
        assert instance.call("stack_sum", n) == s32(sum(i * n for i in range(64)))

    stack_pointer = instance.vmctx[GLOBALS]
    instance.call("stack_sum", 1)
    assert instance.vmctx[GLOBALS] == stack_pointer

    assert instance.call("spin", 100000) == 100000
    assert instance.stack_checks == 0
    # A limit above any stack pointer makes the code call stack_check, which
    # lowers it again.
    instance.vmctx[STACK_LIMIT] = 2**64 - 1
    assert instance.call("spin", 1000) == 1000
    assert instance.stack_checks == 1

    assert instance.call("dispatch", 0, 20, 22) == 42
    assert instance.call("dispatch", 1, 6, 7) == 42
    assert instance.call("dispatch", 2, 40, 2) == 1042
    assert instance.call("call_kernel", 3, 4) == 3021
    assert kernel_calls == [(40, 2), (3, 4)]

    negate = instance.call("unary_pointer", 0)
    twice = instance.call("unary_pointer", 1)
    assert instance.call("call_unary", negate, 5) == -5
    assert instance.call("call_unary", twice, 5) == 10
    assert instance.run("call_unary", instance.call("binary_pointer"), 5).code == SIGNATURE
    assert instance.run("call_unary", 0, 5).code == SIGNATURE
    assert instance.run("call_unary", instance.vmctx[TABLE_SIZE], 5).code == TABLE
    assert instance.run("unreachable").code == UNREACHABLE


def main():
    kernel_calls = []

    def add(a, b):
        kernel_calls.append((a, b))

        return a + b + 1000

    instance = Instance(Path(sys.argv[1]).read_bytes(), Path(sys.argv[2]).read_bytes(), {"kernel.add": add})
    count = 0

    for name, args, expected in cases():
        check(instance, name, args, expected)
        count += 1

    assert count > 5000, count
    test_memory(instance)
    test_control(instance, kernel_calls)


if __name__ == "__main__":
    main()
