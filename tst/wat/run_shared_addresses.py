#!/usr/bin/env python3
"""Copy through shared address expressions with late constant coefficients."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def program(template):
    prefix, rest = template.split(";; BEGIN_FUNCTION")
    body, suffix = rest.split(";; END_FUNCTION")
    functions, checks = [], []
    image = bytes((index * 37 + 11) % 251 for index in range(256))
    count_checks = 0
    for depth in (4, 8, 12, 16):
        function = body.replace("$copy", f"$copy{depth}")
        locals = " ".join(f"(local $p{i} i32)" for i in range(depth))
        locals += " ".join(f"(local $g{i} i32)" for i in range(17))
        function = function.replace(";; LOCALS", locals)
        function = function.replace(";; INITIAL", " ".join(["(i32.const 0)"] * depth))
        function = function.replace(";; PARAMETERS", "(param " + " ".join(["i32"] * depth) + ")")
        function = function.replace(";; RESTORE", " ".join(f"(local.set $p{i})" for i in reversed(range(depth))))
        address = "\n".join(f"(local.set $g{i + 1} (i32.add (local.get $g{i}) "
                            f"(i32.mul (local.get $g{i}) (local.get $p0))))" for i in range(16))
        function = function.replace(";; ADDRESS", address)
        rotation = " ".join(f"(local.get $p{i})" for i in range(1, depth)) + " (i32.const 0)"
        function = function.replace(";; ROTATION", rotation).replace(";; DROP", " ".join(["drop"] * depth))
        functions.append(function)
        for count in (0, 1, 17, 97):
            checks.extend((f"(memory.fill (i32.const 32767) (i32.const 85) (i32.const {count + 2}))",
                           f"(if (i32.ne (call $copy{depth} (i32.const 32768) (i32.const 8192) "
                           f"(i32.const {count})) (i32.const {count})) (then unreachable))"))
            count_checks += 1
            for offset in range(-1, count + 1):
                expected = image[offset] if 0 <= offset < count else 85
                checks.append(f"(if (i32.ne (i32.load8_u (i32.const {32768 + offset})) "
                              f"(i32.const {expected})) (then unreachable))")
                count_checks += 1
    data = "".join(f"\\{byte:02x}" for byte in image)
    suffix = suffix.replace(";; DATA", f'(data (i32.const 8192) "{data}")')
    text = prefix + "\n".join(functions) + suffix.rstrip()[:-1]
    return text + '\n(func (export "_start")\n' + "\n".join(checks) + "\n))\n", count_checks


def main():
    vm, native, wazero, wat2wasm, fixture = sys.argv[1:]
    text, count = program(Path(fixture).read_text())
    with tempfile.TemporaryDirectory(prefix="pizdos-wat-shared-addresses-") as directory:
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
    print(f"WAT shared addresses: {count} byte and boundary checks agree with an integer model, wazero and both AOTs")


if __name__ == "__main__":
    main()
