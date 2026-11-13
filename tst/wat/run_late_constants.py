#!/usr/bin/env python3
"""Keep chains of loop parameters explicit as they gradually become constants."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def program(template):
    prefix, rest = template.split(";; BEGIN_FUNCTION")
    body, rest = rest.split(";; END_FUNCTION")
    middle, rest = rest.split(";; BEGIN_COPY_FUNCTION")
    copy_body, suffix = rest.split(";; END_COPY_FUNCTION")
    functions, checks = [], []
    for depth in (8, 12, 16, 20, 32):
        function = body.replace("$scan", f"$scan{depth}")
        function = function.replace(";; LOCALS", " ".join(f"(local $p{i} i32)" for i in range(depth)))
        function = function.replace(";; INITIAL", " ".join(["(i32.const 37)"] * depth))
        function = function.replace(";; PARAMETERS", "(param " + " ".join(["i32"] * depth) + ")")
        function = function.replace(";; RESTORE", " ".join(f"(local.set $p{i})" for i in reversed(range(depth))))
        reads = []
        for i in range(depth):
            reads.append(f"""(local.set $sum (i32.add (local.get $sum)
                (i32.load8_u (i32.add (local.get $p{i}) (local.get $base)))))
              (if (i32.and (local.get $p{i}) (local.get $bits))
                (then (global.set $tag (i32.add (global.get $tag) (i32.const 1))))
                (else (global.set $tag (i32.sub (global.get $tag) (i32.const 1)))))""")
        function = function.replace(";; READS", "\n".join(reads))
        rotation = " ".join(f"(local.get $p{i})" for i in range(1, depth)) + " (i32.const 37)"
        function = function.replace(";; ROTATION", rotation).replace(";; DROP", " ".join(["drop"] * depth))
        functions.append(function)
        for count in (1, 2, 17):
            for bits in (0, 1, 2, 37, 0xffffffff):
                # All carried values remain 37: each pass adds byte 19 and
                # one signed tag for each parameter, independent of rotation.
                expected = count * depth * (19 + (1 if bits & 37 else -1))
                checks.append(f"(if (i32.ne (call $scan{depth} (i32.const 8192) (i32.const {count}) "
                              f"(i32.const {bits})) (i32.const {expected})) (then unreachable))")
    assertions = len(checks)
    copies = []
    image = bytes((index * 37 + 11) % 251 for index in range(256))
    for depth in (4, 8, 12, 16):
        function = copy_body.replace("$collapse", f"$collapse{depth}")
        function = function.replace(";; LOCALS", " ".join(f"(local $p{i} i32)" for i in range(depth)))
        function = function.replace(";; INITIAL", " ".join(["(i32.const 0)"] * depth))
        function = function.replace(";; PARAMETERS", "(param " + " ".join(["i32"] * depth) + ")")
        function = function.replace(";; RESTORE", " ".join(f"(local.set $p{i})" for i in reversed(range(depth))))
        rotation = " ".join(f"(local.get $p{i})" for i in range(1, depth)) + " (i32.const 0)"
        function = function.replace(";; ROTATION", rotation).replace(";; DROP", " ".join(["drop"] * depth))
        copies.append(function)
        for count in (0, 1, 17, 97):
            expected = image[count - 1] if count else 85
            checks.extend(("(i32.store (i32.const 32767) (i32.const 1431655765))",
                           f"(if (i32.ne (call $collapse{depth} (i32.const 32768) (i32.const 12288) "
                           f"(i32.const {count})) (i32.const {count})) (then unreachable))",
                           f"(if (i32.ne (i32.load (i32.const 32767)) "
                           f"(i32.const {0x55550055 + (expected << 8)})) (then unreachable))"))
            assertions += 2
    data = "".join(f"\\{byte:02x}" for byte in image)
    suffix = suffix.replace(";; COPY_DATA", f'(data (i32.const 12288) "{data}")')
    text = prefix + "\n".join(functions) + middle + "\n".join(copies) + suffix.rstrip()[:-1]
    return text + '\n(func (export "_start")\n' + "\n".join(checks) + "\n))\n", assertions


def main():
    vm, native, wazero, wat2wasm, fixture = sys.argv[1:]
    source_text, count = program(Path(fixture).read_text())
    with tempfile.TemporaryDirectory(prefix="pizdos-wat-late-constants-") as directory:
        work = Path(directory)
        source, module, compiled, embedded = (work / name for name in ("input.wat", "input.wasm", "native.o", "embedded.o"))
        source.write_text(source_text)
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
            result = subprocess.run([vm, "run", "--", str(module)], capture_output=True, env=env, timeout=30)
            assert (result.returncode, result.stdout, result.stderr) == (0, b"", b""), (strict, result)
    print(f"WAT late constants: {count} checks of carried parameters and zero-coefficient repeated stores agree with an integer model, wazero and both AOTs")


if __name__ == "__main__":
    main()
