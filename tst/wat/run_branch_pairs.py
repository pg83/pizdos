#!/usr/bin/env python3
"""Combine comparisons while keeping deferred arithmetic and live arguments."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

MASK = 0xffffffff


def rotate(value):
    return ((value << 3) | (value >> 29)) & MASK


def program(template):
    prefix, rest = template.split(";; BEGIN_FUNCTION")
    body, suffix = rest.split(";; END_FUNCTION")
    functions, checks, count = [], [], 0
    operations = {
        "mask": ("(i32.and (local.get $y) (i32.const 255))", lambda y, z: y & 255),
        "xor": ("(i32.xor (local.get $y) (i32.const 255))", lambda y, z: y ^ 255),
        "shift": ("(i32.shl (local.get $y) (local.get $z))", lambda y, z: (y << (z & 31)) & MASK),
        "divide": ("(i32.div_u (local.get $y) (i32.or (local.get $z) (i32.const 1)))", lambda y, z: y // (z | 1)),
    }
    cases = ((0, 0, 0), (17, 0, 0), (18, 256, 0), (0, 255, 255),
             (0, 258, 2), (18, 258, 2), (1, MASK, 31), (17, 0x80000000, 17),
             (0, 258, 509), (18, 258, 509), (0, 255, 0), (18, 255, 0),
             (0, 32, 32), (18, 32, 32), (0, 9, 3), (18, 9, 3))
    for _, model in operations.values():
        assert {(x < 17, model(y, z) == z) for x, y, z in cases} == {
            (False, False), (False, True), (True, False), (True, True)}
    for width in (0, 8, 16, 24):
        payload = [(index + 1) * 101 for index in range(width)]
        parameters = " ".join("(param i32)" for _ in payload)
        arguments = " ".join(f"(i32.const {value})" for value in payload)
        # Ordered hashing makes a lost or permuted live argument observable.
        result = "(global.get $tag)"
        for index in range(width):
            result = f"(i32.xor (i32.rotl {result} (i32.const 3)) (local.get {index + 3}))"
        for conjunction in (False, True):
            for operation, (expression, model) in operations.items():
                name = f"choose_{width}_{int(conjunction)}_{operation}"
                function = body.replace("$choose", "$" + name).replace(";; PARAMETERS", parameters)
                function = function.replace(";; BOOLEAN", "i32.and" if conjunction else "i32.or")
                functions.append(function.replace(";; OPERATION", expression).replace(";; RESULT", result))
                for x, y, z in cases:
                    right = model(y, z) == z
                    branch = (x < 17 and right) if conjunction else (x < 17 or right)
                    expected = 123 if branch else 45
                    for value in payload:
                        expected = rotate(expected) ^ value
                    checks.append(f"(if (i32.ne (call ${name} (i32.const {x}) (i32.const {y}) "
                                  f"(i32.const {z}) {arguments}) (i32.const {expected})) (then unreachable))")
                    count += 1
    text = prefix + "\n".join(functions) + suffix.rstrip()[:-1]
    return text + '\n(func (export "_start")\n' + "\n".join(checks) + "\n))\n", count


def main():
    vm, native, wazero, wat2wasm, fixture = sys.argv[1:]
    text, count = program(Path(fixture).read_text())
    with tempfile.TemporaryDirectory(prefix="pizdos-wat-branch-pairs-") as directory:
        work = Path(directory)
        source, module, compiled, embedded = (work / name for name in ("input.wat", "input.wasm", "native.o", "embedded.o"))
        source.write_text(text)
        result = subprocess.run([wat2wasm, str(source), "-o", str(module)], capture_output=True, timeout=30)
        assert result.returncode == 0, result.stderr.decode()
        result = subprocess.run([wazero, "-interpreter", str(module)], capture_output=True, timeout=30)
        assert result.returncode == 0 and result.stdout == b"", result
        for strict in (False, True):
            env = dict(os.environ, W2X_MEMORY_TAIL="strict" if strict else "optimized", W2X_TAIL_PROFILE="")
            result = subprocess.run([native, *(["--strict"] if strict else [])], input=module.read_bytes(), capture_output=True, env=env, timeout=60)
            assert result.returncode == 0, (strict, result.stderr)
            compiled.write_bytes(result.stdout)
            result = subprocess.run([vm, "-o", str(embedded), str(module)], capture_output=True, env=env, timeout=60)
            assert result.returncode == 0 and embedded.read_bytes() == compiled.read_bytes(), (strict, result)
            for obj in (compiled, embedded):
                result = subprocess.run([vm, "run", "--", str(obj)], capture_output=True, env=env, timeout=30)
                assert (result.returncode, result.stdout, result.stderr) == (0, b"", b""), (strict, result)
    print(f"WAT branch pairs: {count} AND/OR conditions with deferred arithmetic and up to 24 live arguments agree with an integer model, wazero and both AOTs")


if __name__ == "__main__":
    main()
