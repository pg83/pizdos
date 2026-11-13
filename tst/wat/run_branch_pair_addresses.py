#!/usr/bin/env python3
"""Check paired comparisons with deferred memory-address arithmetic."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

MASK = 0xffffffff


def function(body, name, typ, offset, conjunction):
    body = body.replace("$choose", "$" + name)
    body = body.replace("(param $wanted i32)", f"(param $wanted {typ})")
    body = body.replace("(i32.eq", f"({typ}.eq").replace("(i32.load", f"({typ}.load")
    body = body.replace(";; OFFSET\n", str(offset))
    return body.replace(";; BOOLEAN", "i32.and" if conjunction else "i32.or")


def program(template):
    prefix, rest = template.split(";; BEGIN_FUNCTION")
    body, suffix = rest.split(";; END_FUNCTION")
    functions, checks, count = [], [], 0
    for width in (4, 8):
        typ = "i32" if width == 4 else "i64"
        mask = (1 << (8 * width)) - 1
        seeds = (0, 1, 1 << (8 * width - 1), mask)
        for offset in (1025, 4095, 8192):
            for conjunction in (False, True):
                name = f"choose_{width}_{offset}_{int(conjunction)}"
                functions.append(function(body, name, typ, offset, conjunction))
                for base in (2048, 2052, 2080):
                    address = base + offset
                    for seed in seeds:
                        for matches in (False, True):
                            wanted = seed if matches else seed ^ 1
                            for x in (0, 16, 17, MASK):
                                branch = (x < 17 and matches) if conjunction else (x < 17 or matches)
                                expected = (123 if branch else 45) ^ ((x + base) & MASK)
                                checks.extend((
                                    f"(i32.store8 (i32.const {address - 1}) (i32.const 165))",
                                    f"(i32.store8 (i32.const {address + width}) (i32.const 90))",
                                    f"({typ}.store (i32.const {address}) ({typ}.const {seed}))",
                                    f"(if (i32.ne (call ${name} (i32.const {x}) (i32.const {base}) "
                                    f"({typ}.const {wanted})) (i32.const {expected})) (then unreachable))",
                                    f"(if ({typ}.ne ({typ}.load (i32.const {address})) "
                                    f"({typ}.const {seed})) (then unreachable))",
                                    f"(if (i32.ne (i32.load8_u (i32.const {address - 1})) "
                                    "(i32.const 165)) (then unreachable))",
                                    f"(if (i32.ne (i32.load8_u (i32.const {address + width})) "
                                    "(i32.const 90)) (then unreachable))",
                                ))
                                count += 1
    text = prefix + "\n".join(functions) + suffix.rstrip()[:-1]
    return text + '\n(func (export "_start")\n' + "\n".join(checks) + "\n))\n", count


def trap_programs(template):
    prefix, rest = template.split(";; BEGIN_FUNCTION")
    body, _ = rest.split(";; END_FUNCTION")
    for typ in ("i32", "i64"):
        for offset in (1025, 4095, 8192):
            for conjunction in (False, True):
                for x in (0, 18):
                    label = typ, offset, conjunction, x
                    # Both operands of Wasm AND/OR are evaluated. Even when
                    # x determines its result, the load must still trap.
                    text = prefix + function(body, "choose", typ, offset, conjunction)
                    text += '(func (export "_start") (drop (call $choose '
                    text += f'(i32.const {x}) (i32.const 65535) ({typ}.const 5)))))\n'
                    yield label, text


def main():
    vm, native, wazero, wat2wasm, fixture = sys.argv[1:]
    template = Path(fixture).read_text()
    text, count = program(template)
    traps = 0
    with tempfile.TemporaryDirectory(prefix="pizdos-wat-branch-addresses-") as directory:
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
            result = subprocess.run([vm, "run", "--", str(module)], capture_output=True, env=env, timeout=30)
            assert (result.returncode, result.stdout, result.stderr) == (0, b"", b""), (strict, result)
        for label, text in trap_programs(template):
            source.write_text(text)
            result = subprocess.run([wat2wasm, str(source), "-o", str(module)], capture_output=True, timeout=30)
            assert result.returncode == 0, (label, result.stderr.decode())
            result = subprocess.run([wazero, "-interpreter", str(module)], capture_output=True, timeout=30)
            assert result.returncode == 1 and b"out of bounds memory access" in result.stderr, (label, result)
            for strict in (False, True):
                env = dict(os.environ, W2X_MEMORY_TAIL="strict" if strict else "optimized", W2X_TAIL_PROFILE="")
                result = subprocess.run([native, *(["--strict"] if strict else [])], input=module.read_bytes(), capture_output=True, env=env, timeout=60)
                assert result.returncode == 0, (label, strict, result.stderr)
                compiled.write_bytes(result.stdout)
                result = subprocess.run([vm, "-o", str(embedded), str(module)], capture_output=True, env=env, timeout=60)
                assert result.returncode == 0 and embedded.read_bytes() == compiled.read_bytes(), (label, strict, result)
                result = subprocess.run([vm, "run", "--", str(module)], capture_output=True, env=env, timeout=30)
                assert result.returncode == 130 and result.stdout == b"" and b"out of bounds memory access" in result.stderr, (label, strict, module.name, result)
            traps += 1
    print(f"WAT branch addresses: {count} modeled comparisons preserve results/memory/sentinels and {traps} eager loads trap, with wazero and both AOTs")


if __name__ == "__main__":
    main()
