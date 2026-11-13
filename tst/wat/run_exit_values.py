#!/usr/bin/env python3
"""Keep direct return and branch uses of loads after loops with early exits."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

IMAGE = bytes((index * 37 + 11) % 251 if index >= 4096 else 0 for index in range(65536))


def program(template, branch):
    after = "(local.get $last)"
    if branch:
        after = """(if (local.get $last)
          (then (global.set $tag (i32.const 7)) (return (i32.const 11))))
          (global.set $tag (i32.const 9))
          (i32.const 13)"""
    template = template.replace(";; AFTER", after)
    image, checks, count = bytearray(IMAGE), [], 0
    for size in (1, 7, 19):
        zero_source = next(index for index in range(8192, 8443) if IMAGE[index + size - 1] == 0)
        for source, dest in ((8192, 32768), (zero_source, 32768), (8192, 8193)):
            for early in (False, True):
                written = []
                for index in range(size):
                    last = image[source + index]
                    image[dest + index] = last
                    written.append(dest + index)
                    if early and index == 5:
                        expected, tag = 123, 99
                        break
                else:
                    expected, tag = (11 if last else 13, 7 if last else 9) if branch else (last, 0)
                checks += [f"(if (i32.ne (call $copy (i32.const {dest}) (i32.const {source}) (i32.const {size}) "
                           f"(i32.const {int(early)})) (i32.const {expected})) (then unreachable))",
                           f"(if (i32.ne (global.get $tag) (i32.const {tag})) (then unreachable))"]
                for location in written + [dest - 1, dest + size]:
                    checks.append(f"(if (i32.ne (i32.load8_u (i32.const {location})) (i32.const {image[location]})) (then unreachable))")
                count += 1
    data = "".join(f"\\{byte:02x}" for byte in IMAGE[4096:])
    return template.rstrip()[:-1] + f'\n(data (i32.const 4096) "{data}")\n(func (export "_start")\n' + "\n".join(checks) + "\n))\n", count


def main():
    vm, native, wazero, wat2wasm, fixture = sys.argv[1:]
    template, checked = Path(fixture).read_text(), 0
    with tempfile.TemporaryDirectory(prefix="pizdos-wat-exit-values-") as directory:
        work = Path(directory)
        source, module, compiled, embedded = (work / name for name in ("input.wat", "input.wasm", "native.o", "embedded.o"))
        for branch in (False, True):
            wat, count = program(template, branch)
            source.write_text(wat)
            result = subprocess.run([wat2wasm, str(source), "-o", str(module)], capture_output=True, timeout=30)
            assert result.returncode == 0, result.stderr.decode()
            result = subprocess.run([wazero, "-interpreter", str(module)], capture_output=True, timeout=30)
            assert result.returncode == 0 and result.stdout == b"", (branch, result)
            for strict in (False, True):
                env = dict(os.environ, W2X_MEMORY_TAIL="strict" if strict else "optimized", W2X_TAIL_PROFILE="")
                result = subprocess.run([native, *(["--strict"] if strict else [])], input=module.read_bytes(), capture_output=True, env=env, timeout=60)
                assert result.returncode == 0, (branch, strict, result.stderr)
                compiled.write_bytes(result.stdout)
                result = subprocess.run([vm, "-o", str(embedded), str(module)], capture_output=True, env=env, timeout=60)
                assert result.returncode == 0 and embedded.read_bytes() == compiled.read_bytes(), (branch, strict, result)
                result = subprocess.run([vm, "run", "--", str(module)], capture_output=True, env=env, timeout=30)
                assert (result.returncode, result.stdout, result.stderr) == (0, b"", b""), (branch, strict, result)
            checked += count
    print(f"WAT exit values: {checked} early/normal exits preserve returned and branched-on loads with a bytewise model, wazero and both AOTs")


if __name__ == "__main__":
    main()
