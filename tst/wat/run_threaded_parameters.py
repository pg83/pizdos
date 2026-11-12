#!/usr/bin/env python3
"""Drop interleaved parameters before threading known and unknown table keys."""
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
    for width in (0, 3, 8, 24):
        name, arity = f"choose_{width}", 2 * width + 1
        entries = []
        for entry, key in ((1, "(i32.const -1)"), (2, "(i32.const 7)"),
                           (3, "(i32.const 1)"), (4, "(local.get $seed)"), (0, "(i32.const 0)")):
            values = []
            for index in range(width):
                salt = entry * 1009 + (index + 1) * 97
                # The first value of every pair is unused. Its parameter is
                # removed while its live neighbour must retain its value.
                values.extend((f"(i32.const {index + 1})",
                               f"(i32.xor (local.get $seed) (i32.const {salt}))"))
            values.append(key)
            if entry:
                entries.extend(("(br_if $join " + " ".join(values) +
                                f" (i32.eq (local.get $selector) (i32.const {entry})))", "drop\n" * arity))
            else:
                entries.extend(values)
        result = "(i32.add (local.get $seed) (local.get $key))"
        for index in range(width):
            result = f"(i32.xor (i32.rotl {result} (i32.const 3)) (local.get $value{index}))"
        function = body.replace("$choose", "$" + name)
        function = function.replace(";; LOCALS", "\n".join(f"(local $value{index} i32)" for index in range(width)))
        function = function.replace(";; TYPES", " ".join("i32" for _ in range(arity)))
        function = function.replace(";; ENTRIES", "\n".join(entries))
        function = function.replace(";; TAKE_VALUES", "\n".join(f"(local.set $value{index})\ndrop" for index in reversed(range(width))))
        functions.append(function.replace(";; HASH", result))
        for selector in (0, 1, 2, 3, 4, MASK):
            for seed in (0, 1, 17, 0x7fffffff, 0x80000000, MASK):
                entry = selector if 1 <= selector <= 4 else 0
                key = {0: 0, 1: MASK, 2: 7, 3: 1, 4: seed}[entry]
                expected = (seed + key) & MASK
                for index in range(width):
                    expected = rotate(expected) ^ (seed ^ (entry * 1009 + (index + 1) * 97))
                if key == 0:
                    expected, tag = expected ^ 0xa5a5a5a5, 10
                elif key == 1:
                    expected, tag = (expected * 33) & MASK, 20
                else:
                    expected, tag = (expected - 17) & MASK, 30
                checks.extend((f"(if (i32.ne (call ${name} (i32.const {selector}) (i32.const {seed})) "
                               f"(i32.const {expected})) (then unreachable))",
                               f"(if (i32.ne (global.get $tag) (i32.const {tag})) (then unreachable))"))
                count += 1
    text = prefix + "\n".join(functions) + suffix.rstrip()[:-1]
    return text + '\n(func (export "_start")\n' + "\n".join(checks) + "\n))\n", count


def main():
    vm, native, wazero, wat2wasm, fixture = sys.argv[1:]
    text, count = program(Path(fixture).read_text())
    with tempfile.TemporaryDirectory(prefix="pizdos-wat-threaded-parameters-") as directory:
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
    print(f"WAT threaded parameters: {count} known/unknown table keys retain up to 24 values after dropping interleaved parameters, with a model, wazero and both AOTs")


if __name__ == "__main__":
    main()
