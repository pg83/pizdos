#!/usr/bin/env python3
"""Rotate checked and unchecked addresses through long cyclic parameter chains."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def program(template, trap_depth=None):
    prefix, rest = template.split(";; BEGIN_FUNCTION")
    body, suffix = rest.split(";; END_FUNCTION")
    functions, checks = [], []
    for depth in (32, 64, 96, 128):
        types = " ".join(["i32"] * depth)
        function = body.replace("$rotate", f"$rotate{depth}")
        function = function.replace(";; LOCALS", " ".join(f"(local $p{i} i32)" for i in range(depth)))
        function = function.replace(";; INITIAL", " ".join(["(local.get $a)"] * (depth - 1) + ["(local.get $b)"]))
        function = function.replace(";; PARAMETERS", f"(param {types}) (result {types})")
        restore = " ".join(f"(local.set $p{i})" for i in reversed(range(depth)))
        function = function.replace(";; RESTORE", restore)
        rotation = " ".join(f"(local.get $p{i})" for i in range(1, depth)) + " (local.get $p0)"
        functions.append(function.replace(";; ROTATION", rotation))
        for count in (1, 2, depth - 2, depth - 1, depth, depth + 1, 2 * depth - 1):
            expected = 231 if count % depth == depth - 1 else 19
            checks.append(f"(if (i32.ne (call $rotate{depth} (i32.const 8192) (i32.const 12288) "
                          f"(i32.const {count})) (i32.const {expected})) (then unreachable))")
        for count in (1, depth - 2):
            checks.append(f"(if (i32.ne (call $rotate{depth} (i32.const 8192) (i32.const 131072) "
                          f"(i32.const {count})) (i32.const 19)) (then unreachable))")
    entry = "\n".join(checks)
    if trap_depth is not None:
        entry = (f"(drop (call $rotate{trap_depth} (i32.const 8192) (i32.const 131072) "
                 f"(i32.const {trap_depth - 1})))")
    text = prefix + "\n".join(functions) + suffix.rstrip()[:-1]
    return text + '\n(func (export "_start")\n' + entry + "\n))\n", len(checks)


def main():
    vm, native, wazero, wat2wasm, fixture = sys.argv[1:]
    template = Path(fixture).read_text()
    with tempfile.TemporaryDirectory(prefix="pizdos-wat-address-rotation-") as directory:
        work = Path(directory)
        source, module, compiled, embedded = (work / name for name in ("input.wat", "input.wasm", "native.o", "embedded.o"))
        for trap_depth in (None, 32, 128):
            text, count = program(template, trap_depth)
            source.write_text(text)
            result = subprocess.run([wat2wasm, str(source), "-o", str(module)], capture_output=True, timeout=30)
            assert result.returncode == 0, result.stderr.decode()
            result = subprocess.run([wazero, "-interpreter", str(module)], capture_output=True, timeout=30)
            if trap_depth is None:
                assert result.returncode == 0 and result.stdout == b"", result
            else:
                assert result.returncode != 0 and b"out of bounds memory access" in result.stderr, result
            for strict in (False, True):
                env = dict(os.environ, W2X_MEMORY_TAIL="strict" if strict else "optimized", W2X_TAIL_PROFILE="")
                result = subprocess.run([native, *(["--strict"] if strict else [])], input=module.read_bytes(), capture_output=True, env=env, timeout=60)
                assert result.returncode == 0, (strict, result.stderr)
                compiled.write_bytes(result.stdout)
                result = subprocess.run([vm, "-o", str(embedded), str(module)], capture_output=True, env=env, timeout=60)
                assert result.returncode == 0 and embedded.read_bytes() == compiled.read_bytes(), (strict, result)
                result = subprocess.run([vm, "run", "--", str(module)], capture_output=True, env=env, timeout=30)
                if trap_depth is None:
                    assert (result.returncode, result.stdout, result.stderr) == (0, b"", b""), (strict, result)
                else:
                    assert result.returncode == 130 and result.stdout == b"", (strict, result)
                    assert result.stderr.startswith(b"wasm trap: out of bounds memory access\n") and b"panic:" not in result.stderr, result
    print(f"WAT address rotation: {count} address checks and short/long out-of-bounds chains agree with a rotation model, wazero and both AOTs")


if __name__ == "__main__":
    main()
