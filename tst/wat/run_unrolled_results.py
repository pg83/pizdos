#!/usr/bin/env python3
"""Keep duplicate branch payloads and return values after fixed memory loops."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

MASK = 0xffffffff


def program(template):
    prefix, rest = template.split(";; BEGIN_FUNCTION")
    body, suffix = rest.split(";; END_FUNCTION")
    functions, checks, count = [], [], 0
    for trips in (2, 3, 4, 8):
        name = f"channels_{trips}"
        functions.append(body.replace("$channels", "$" + name).replace(";; TRIPS", str(trips)))
        for selector in (0, 1, 2, 3, MASK):
            for seed in (0, 1, 17, 0x7fffffff, 0x80000000, MASK):
                total, data = seed, []
                for i in range(trips):
                    byte = (seed + i * 17) & 255
                    data.append(byte)
                    total = ((total * 33) ^ byte) & MASK
                changed = total ^ 165 if selector == 0 else (total * 33) & MASK if selector == 1 else total
                changed = (changed - 17) & MASK
                checks.extend(("(memory.fill (i32.const 8191) (i32.const 85) (i32.const 10))",
                               f"(call ${name} (i32.const 8192) (i32.const {seed}) (i32.const {selector}))",
                               "(local.set $changed)", "(local.set $original)",
                               f"(if (i32.ne (local.get $original) (i32.const {total})) (then unreachable))",
                               f"(if (i32.ne (local.get $changed) (i32.const {changed})) (then unreachable))"))
                for i, byte in [(-1, 85), *enumerate(data), (trips, 85)]:
                    checks.append(f"(if (i32.ne (i32.load8_u (i32.const {8192+i})) (i32.const {byte})) (then unreachable))")
                count += 1
    text = prefix + "\n".join(functions) + suffix.rstrip()[:-1]
    text += '\n(func (export "_start") (local $original i32) (local $changed i32)\n'
    return text + "\n".join(checks) + "\n))\n", count


def main():
    vm, native, wazero, wat2wasm, fixture = sys.argv[1:]
    text, count = program(Path(fixture).read_text())
    with tempfile.TemporaryDirectory(prefix="pizdos-wat-unrolled-results-") as directory:
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
    print(f"WAT unrolled results: {count} modeled memory loops retain duplicate branch payloads and two ordered results, with wazero and both AOTs in strict/optimized modes")


if __name__ == "__main__":
    main()
