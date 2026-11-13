#!/usr/bin/env python3
"""Carry integers and floats through explicit multi-latch dispatch loops."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

MASK = (1 << 64) - 1


def literal(value):
    return str(value if value < 1 << 63 else value - (1 << 64))


def dispatch_body(handlers, width, constant_steps, nested=False, inner=False):
    prefix = "inner" if inner else ""
    key_name, rounds_name = "$" + prefix + "key", "$" + prefix + "rounds"
    body = "".join(f"(block ${prefix}case{index}\n" for index in reversed(range(handlers)))
    body += "(br_table " + " ".join(f"${prefix}case{index}" for index in range(handlers)) + f" ${prefix}case{handlers - 1} (local.get {key_name}))\n"
    for index in range(handlers):
        variable, salt = index % width, index + (101 if inner else 1)
        body += f")\n(local.set $x{variable} (call $mix (local.get $x{variable}) (i64.const {salt})))\n"
        body += f"(local.set $f{variable} (call $bump (local.get $f{variable}) (i32.const {salt})))\n"
        if nested and index == 0:
            body += "(local.set $innerkey (i32.wrap_i64 (local.get $seed)))\n(local.set $innerrounds (i32.const 3))\n"
            body += "(block $innerdone (loop $inneragain\n" + dispatch_body(17, width, constant_steps, inner=True) + "))\n"
        key = f"(i32.const {(index + 1) % handlers})" if constant_steps else f"(i32.xor (i32.add (local.get {key_name}) (i32.wrap_i64 (local.get $seed))) (i32.const {index + 3}))"
        body += f"(local.set {key_name} {key})\n(local.set {rounds_name} (i32.sub (local.get {rounds_name}) (i32.const 1)))\n"
        body += f"(br_if ${prefix}again (local.get {rounds_name}))\n(br ${prefix}done)\n"
    return body


def program(template, handlers, width, constant_steps, nested=False):
    locals, initial = [], []
    if nested:
        locals += ["(local $innerkey i32)", "(local $innerrounds i32)"]
    for index in range(width):
        locals += [f"(local $x{index} i64)", f"(local $f{index} f64)"]
        initial += [f"(local.set $x{index} (i64.add (local.get $seed) (i64.const {index + 1})))",
                    f"(local.set $f{index} (f64.convert_i64_u (i64.add (i64.and (local.get $seed) (i64.const 4095)) (i64.const {index + 1}))))"]
    body = dispatch_body(handlers, width, constant_steps, nested)
    result = "(i64.const 0)"
    for index in range(width):
        result = f"(i64.xor (i64.add {result} (local.get $x{index})) (i64.trunc_f64_u (local.get $f{index})))"
    template = template.replace(";; LOCALS", "\n".join(locals)).replace(";; INITIALIZE", "\n".join(initial))
    template = template.replace(";; DISPATCH", body).replace(";; RESULT", result)
    checks = []
    for start in (0, handlers - 1, 0xffffffff):
        for rounds in (0, 1, 2, 37):
            for seed in (0, 17, 0xffffffffffffffff):
                integers = [(seed + index + 1) & MASK for index in range(width)]
                floats = [(seed & 4095) + index + 1 for index in range(width)]
                key = start
                for _ in range(rounds):
                    index = min(key, handlers - 1)
                    variable = index % width
                    integers[variable] = ((integers[variable] * 33) ^ (index + 1)) & MASK
                    floats[variable] += index + 1
                    if nested and index == 0:
                        inner_key = seed & 0xffffffff
                        for _ in range(3):
                            inner_index = min(inner_key, 16)
                            variable = inner_index % width
                            salt = inner_index + 101
                            integers[variable] = ((integers[variable] * 33) ^ salt) & MASK
                            floats[variable] += salt
                            inner_key = (inner_index + 1) % 17 if constant_steps else (((inner_key + (seed & 0xffffffff)) & 0xffffffff) ^ (inner_index + 3))
                    key = (index + 1) % handlers if constant_steps else (((key + (seed & 0xffffffff)) & 0xffffffff) ^ (index + 3))
                expected = 0
                for integer, floating in zip(integers, floats):
                    expected = ((expected + integer) & MASK) ^ floating
                checks.append(f"(if (i64.ne (call $dispatch (i32.const {start}) (i32.const {rounds}) (i64.const {literal(seed)})) "
                              f"(i64.const {literal(expected)})) (then unreachable))")
    return template.rstrip()[:-1] + '\n(func (export "_start")\n' + "\n".join(checks) + "\n))\n", len(checks)


def main():
    vm, native, wazero, wat2wasm, fixture = sys.argv[1:]
    template = Path(fixture).read_text()
    checks = 0
    with tempfile.TemporaryDirectory(prefix="pizdos-wat-dispatch-values-") as directory:
        work = Path(directory)
        source, module, compiled, embedded = (work / name for name in ("input.wat", "input.wasm", "native.o", "embedded.o"))
        for handlers in (17, 33):
            for width in (1, 7, 15, 17, 32, 40):
                for constant_steps, nested in ((False, False), (True, False), (False, True), (True, True)):
                    case = handlers, width, constant_steps, nested
                    wat, count = program(template, *case)
                    checks += count
                    source.write_text(wat)
                    result = subprocess.run([wat2wasm, str(source), "-o", str(module)], capture_output=True, timeout=30)
                    assert result.returncode == 0, (case, result.stderr.decode())
                    result = subprocess.run([wazero, "-interpreter", str(module)], capture_output=True, timeout=30)
                    assert result.returncode == 0 and result.stdout == b"", (case, result)
                    for strict in (False, True):
                        env = dict(os.environ, W2X_MEMORY_TAIL="strict" if strict else "optimized", W2X_TAIL_PROFILE="")
                        result = subprocess.run([native, *(["--strict"] if strict else [])], input=module.read_bytes(), capture_output=True, env=env, timeout=60)
                        assert result.returncode == 0, (case, strict, result.stderr)
                        compiled.write_bytes(result.stdout)
                        result = subprocess.run([vm, "-o", str(embedded), str(module)], capture_output=True, env=env, timeout=60)
                        assert result.returncode == 0 and embedded.read_bytes() == compiled.read_bytes(), (case, strict, result)
                        result = subprocess.run([vm, "run", "--", str(module)], capture_output=True, env=env, timeout=30)
                        assert (result.returncode, result.stdout, result.stderr) == (0, b"", b""), (case, strict, result)
    print(f"WAT dispatch values: {checks} flat/nested state machines with 17/33 latches preserve up to 40 integer and 40 float values across calls, wazero and both AOTs")


if __name__ == "__main__":
    main()
