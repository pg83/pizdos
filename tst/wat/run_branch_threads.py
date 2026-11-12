#!/usr/bin/env python3
"""Thread constant incoming keys through a join used only by br_table."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def program(template):
    checks, count = [], 0
    for selector in (0, 1, 2, 3, 4, 0xffffffff):
        for value in (0, 1, 17, 0x7fffffff, 0x80000000, 0xffffffff):
            if selector in (1, 2):
                expected, tag = (value - 17) & 0xffffffff, 30
            elif selector == 3:
                expected, tag = (value * 33) & 0xffffffff, 20
            else:
                expected, tag = value ^ 0xa5a5a5a5, 10
            checks += [f"(if (i32.ne (call $choose (i32.const {selector}) (i32.const {value})) "
                       f"(i32.const {expected})) (then unreachable))",
                       f"(if (i32.ne (global.get $tag) (i32.const {tag})) (then unreachable))"]
            count += 1
    return template.rstrip()[:-1] + '\n(func (export "_start")\n' + "\n".join(checks) + "\n))\n", count


def main():
    vm, native, wazero, wat2wasm, fixture = sys.argv[1:]
    wat, count = program(Path(fixture).read_text())
    with tempfile.TemporaryDirectory(prefix="pizdos-wat-branch-threads-") as directory:
        work = Path(directory)
        source, module, compiled, embedded = (work / name for name in ("input.wat", "input.wasm", "native.o", "embedded.o"))
        source.write_text(wat)
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
    print(f"WAT branch threads: {count} constant incoming table keys, including unsigned overflow indices, agree with an integer model, wazero and both AOTs")


if __name__ == "__main__":
    main()
