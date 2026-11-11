#!/usr/bin/env python3
"""Integer relations, algebraic identities and narrowing through raw Wasm.

    test_integer_identities.py <vm> <native-aot> <wazero>
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def leb(value, signed=False):
    result = bytearray()
    while True:
        byte = value & 127
        value >>= 7
        if value == 0 and (not signed or byte < 64) or signed and value == -1 and byte >= 64:
            return bytes(result + bytes([byte]))
        result.append(byte | 128)


def section(kind, body):
    return bytes([kind]) + leb(len(body)) + body


def signed(value, bits):
    value &= (1 << bits) - 1
    return value - (1 << bits) if value >= 1 << (bits - 1) else value


def constant(value, bits=32):
    return bytes([0x42 if bits == 64 else 0x41]) + leb(signed(value, bits), True)


def program():
    types, type_ids, functions, entry = [], {}, [], bytearray()
    checks = 0

    def signature(params, result):
        key = tuple(params), result
        if key not in type_ids:
            type_ids[key] = len(types)
            types.append(b"\x60" + leb(len(params)) + bytes(0x7e if bits == 64 else 0x7f for bits in params)
                         + (b"\0" if result is None else b"\1" + bytes([0x7e if result == 64 else 0x7f])))
        return type_ids[key]

    def add(params, result, body, samples, expected):
        nonlocal checks
        index = len(functions)
        functions.append((signature(params, result), b"\0" + body + b"\x0b"))
        for args in samples:
            entry.extend(b"".join(constant(value, bits) for value, bits in zip(args, params)))
            entry.extend(b"\x10" + leb(index) + constant(expected(*args), result))
            entry.extend(bytes([0x52 if result == 64 else 0x47]) + b"\x04\x40\0\x0b")
            checks += 1

    x, y = b"\x20\0", b"\x20\1"
    for bits in (32, 64):
        mask = (1 << bits) - 1
        values = (0, 1, 3, (1 << (bits - 1)) - 1, 1 << (bits - 1), mask - 1, mask)
        samples = [(value,) for value in values]
        for operation in range(4):
            is_signed, remainder = operation in (0, 2), operation >= 2
            divisors = (1, 2, 3, 7, 10, -3, -7, -10, (1 << (bits - 1)) - 1, 1 << (bits - 1), mask)
            for divisor in divisors:
                divisor &= mask
                denominator = signed(divisor, bits) if is_signed else divisor
                valid = [(value,) for value in values
                         if not (is_signed and not remainder and denominator == -1 and value == 1 << (bits - 1))]

                def expected(value):
                    numerator = signed(value, bits) if is_signed else value
                    quotient = abs(numerator) // abs(denominator)
                    if (numerator < 0) != (denominator < 0):
                        quotient = -quotient
                    return numerator - quotient * denominator if remainder else quotient

                add([bits], bits, x + constant(divisor, bits) + bytes([(0x7f if bits == 64 else 0x6d) + operation]), valid, expected)
        predicates = (lambda a, b: a == b, lambda a, b: a != b,
                      lambda a, b: signed(a, bits) < signed(b, bits), lambda a, b: a < b,
                      lambda a, b: signed(a, bits) > signed(b, bits), lambda a, b: a > b,
                      lambda a, b: signed(a, bits) <= signed(b, bits), lambda a, b: a <= b,
                      lambda a, b: signed(a, bits) >= signed(b, bits), lambda a, b: a >= b)
        for relation, predicate in enumerate(predicates):
            for inversions in (1, 2):
                body = x + y + bytes([(0x51 if bits == 64 else 0x46) + relation]) + b"\x45" * inversions
                add([bits, bits], 32, body, [(a, b) for a in values for b in values],
                    lambda a, b: int(bool(predicate(a, b)) ^ bool(inversions % 2)))

            add([bits], 32, x + x + bytes([(0x51 if bits == 64 else 0x46) + relation]), samples,
                lambda a: int(predicate(a, a)))
            add([bits], 32, x + constant(0, bits) + bytes([(0x51 if bits == 64 else 0x46) + relation]), samples,
                lambda a: int(predicate(a, 0)))
        add([bits], 32, x + bytes([0x50 if bits == 64 else 0x45]) + b"\x45", samples, lambda a: int(a != 0))
        offset = 0x7c if bits == 64 else 0x6a
        for opcode, kind, constants in ((0, "add", (0,)), (1, "sub", (0,)), (2, "mul", (0, 1, 2, 4, 8, mask)),
                                        (7, "and", (0, mask)), (8, "or", (0, mask)), (9, "xor", (0, mask))):
            for value in constants:
                for reverse in (False, True):
                    left, right = (constant(value, bits), x) if reverse else (x, constant(value, bits))
                    def expected(a):
                        left_value, right_value = (value, a) if reverse else (a, value)
                        return {"add": left_value + right_value, "sub": left_value - right_value,
                                "mul": left_value * right_value, "and": left_value & right_value,
                                "or": left_value | right_value, "xor": left_value ^ right_value}[kind] & mask
                    add([bits], bits, left + right + bytes([offset + opcode]), samples, expected)
            if kind in ("sub", "and", "or", "xor"):
                add([bits], bits, x + x + bytes([offset + opcode]), samples,
                    lambda a: a if kind in ("and", "or") else 0)
        # Immediate shifts and rotates include real movement and counts
        # reduced modulo the width, including all high bits set.
        for opcode in range(10, 15):
            for amount in (0, 1, 7, bits - 1, bits, mask - (bits - 1)):
                def shifted(a):
                    n = amount % bits
                    if opcode == 10:
                        return (a << n) & mask
                    if opcode == 11:
                        return signed(a, bits) >> n
                    if opcode == 12:
                        return a >> n
                    if opcode == 13:
                        return ((a << n) | (a >> (-n % bits))) & mask
                    return ((a >> n) | (a << (-n % bits))) & mask
                add([bits], bits, x + constant(amount, bits) + bytes([offset + opcode]), samples, shifted)
        for opcode in (3, 4, 5, 6):
            add([bits], bits, x + constant(1, bits) + bytes([offset + opcode]), samples,
                lambda a: a if opcode in (3, 4) else 0)

    operations = ((0x7c, lambda a, b: a + b), (0x7d, lambda a, b: a - b), (0x7e, lambda a, b: a * b),
                  (0x83, lambda a, b: a & b), (0x84, lambda a, b: a | b), (0x85, lambda a, b: a ^ b),
                  (0x86, lambda a, b: a << (b & 63)))
    values = (0, 1, 0x7fffffff, 0x80000000, 0xffffffff)
    for opcode, operation in operations:
        for extend in (0xac, 0xad):
            body = x + bytes([extend]) + y + bytes([extend, opcode, 0xa7])
            def expected(a, b):
                left, right = (signed(a, 32), signed(b, 32)) if extend == 0xac else (a, b)
                return operation(left, right) & 0xffffffff
            add([32, 32], 32, body, [(a, b) for a in values for b in values], expected)

    for extend in (0xac, 0xad):
        samples = [(value,) for value in values]
        add([32], 32, x + bytes([extend, 0xa7]), samples, lambda a: a)
        add([32], 64, x + b"\xad" + constant(0xffffffff, 64) + b"\x83", samples, lambda a: a)
        for amount in (1, 31, 32, 63):
            body = x + bytes([extend]) + constant(amount, 64) + b"\x86\xa7"
            add([32], 32, body, samples,
                lambda a: ((signed(a, 32) if extend == 0xac else a) << amount) & 0xffffffff)

    image = bytes((i * 37 + 129) % 256 for i in range(1024))
    for width, opcode in ((2, 0x2f), (4, 0x28)):
        for amount in ((8,) if width == 2 else (16, 24)):
            body = x + bytes([opcode, 0, 0]) + constant(amount) + b"\x76"
            add([32], 32, body, [(1024 + i,) for i in range(0, len(image) - width, 7)],
                lambda address: int.from_bytes(image[address - 1024:address - 1024 + width], "little") >> amount)

    functions.append((signature([], None), b"\0" + bytes(entry) + b"\x0b"))
    module = b"\0asm\1\0\0\0" + section(1, leb(len(types)) + b"".join(types))
    module += section(3, leb(len(functions)) + b"".join(leb(typ) for typ, _ in functions))
    module += section(5, b"\1\0\1")
    module += section(7, b"\1\6_start\0" + leb(len(functions) - 1))
    module += section(10, leb(len(functions)) + b"".join(leb(len(body)) + body for _, body in functions))
    module += section(11, b"\1\0" + constant(1024) + b"\x0b" + leb(len(image)) + image)
    return module, checks


def main():
    vm, native, wazero = sys.argv[1:]
    data, checks = program()
    with tempfile.TemporaryDirectory(prefix="pizdos-integer-identities-") as directory:
        work = Path(directory)
        source, compiled, embedded = [work / name for name in ("input.wasm", "native.o", "embedded.o")]
        source.write_bytes(data)
        result = subprocess.run([wazero, "-interpreter", str(source)], capture_output=True, timeout=60)
        assert result.returncode == 0 and result.stdout == b"", result
        for strict in (False, True):
            env = dict(os.environ, W2X_MEMORY_TAIL="strict" if strict else "optimized", W2X_TAIL_PROFILE="")
            result = subprocess.run([native, *(["--strict"] if strict else [])], input=data, capture_output=True, env=env, timeout=60)
            assert result.returncode == 0, result.stderr
            compiled.write_bytes(result.stdout)
            result = subprocess.run([vm, "-o", str(embedded), str(source)], capture_output=True, env=env, timeout=60)
            assert result.returncode == 0 and embedded.read_bytes() == compiled.read_bytes(), result
            for obj in (compiled, embedded):
                result = subprocess.run([vm, "run", "--", str(obj)], capture_output=True, env=env, timeout=30)
                assert (result.returncode, result.stdout, result.stderr) == (0, b"", b""), result
    print(f"integer identities: {checks} signed/unsigned relations, constant divisions, modular identities and narrowing agree with wazero and both AOTs")


if __name__ == "__main__":
    main()
