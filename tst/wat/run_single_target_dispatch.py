#!/usr/bin/env python3
"""Keep byte-load narrowing through handlers returning via a one-target table."""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


def program(template):
    checks = []
    for seed in (*range(16), 0x80000080, 0xdeadbeef, 0xffffffff):
        for mode in (0, 1, 0xffffffff):
            for rounds in (1, 2, 5, 17):
                expected = seed & 15
                for _ in range(rounds - 1):
                    expected = (expected * 29 + 3) & 15
                checks.append(f"(if (i32.ne (call $route (i32.const {seed}) "
                              f"(i32.const {mode}) (i32.const {rounds})) "
                              f"(i32.const {expected})) (then unreachable))")
    return template.rstrip()[:-1] + '\n(func (export "_start")\n' + "\n".join(checks) + "\n))\n", len(checks)


def byte_reads(obj, env):
    # Inspect the public ELF with the Go toolchain already used to build VM.
    result = subprocess.run([os.environ.get("GO") or "go", "tool", "objdump", "-s", "^f0$", str(obj)],
                            capture_output=True, env=env, timeout=60)
    assert result.returncode == 0, result.stderr.decode()
    reads = {}
    for line in result.stdout.decode().splitlines():
        match = re.search(r"\s([0-9a-f]+)\s+(\w+)\s+0x([0-9a-f]+)\(R15\),", line)
        if match is None:
            continue
        address = int(match[3], 16)
        if 2048 <= address < 2112 and address % 4 == 0:
            instruction = bytes.fromhex(match[1])
            # Strip the optional x86-64 REX prefix. 0F B6 is MOVZX from a byte;
            # a word/dword read here would leave redundant dispatch masks.
            if 0x40 <= instruction[0] <= 0x4f:
                instruction = instruction[1:]
            reads.setdefault(address, []).append((match[2], instruction[:2].hex()))
    expected = set(range(2048, 2112, 4))
    assert set(reads) == expected, ("missing dispatch reads", reads)
    assert all(opcode == "0fb6" for values in reads.values() for _, opcode in values), ("dispatch reads were not narrowed to bytes", reads)


def main():
    vm, native, wazero, wat2wasm, fixture = sys.argv[1:]
    text, count = program(Path(fixture).read_text())
    with tempfile.TemporaryDirectory(prefix="pizdos-wat-single-target-dispatch-") as directory:
        work = Path(directory)
        source, module, compiled, embedded = (work / name for name in ("input.wat", "input.wasm", "native.o", "embedded.o"))
        source.write_text(text)
        result = subprocess.run([wat2wasm, str(source), "-o", str(module)], capture_output=True, timeout=30)
        assert result.returncode == 0, result.stderr.decode()
        result = subprocess.run([wazero, "-interpreter", str(module)], capture_output=True, timeout=30)
        assert (result.returncode, result.stdout, result.stderr) == (0, b"", b""), result
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
            byte_reads(compiled, env)
    print(f"WAT single-target dispatch: {count} modeled routes agree with wazero and both AOTs; all 16 masked word reads become byte loads in strict/optimized modes")


if __name__ == "__main__":
    main()
