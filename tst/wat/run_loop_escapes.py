#!/usr/bin/env python3
"""Preserve escaped indirect reads across several explicit copy loops."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

IMAGE = bytes((i * 37 + 11) % 251 if i >= 4096 else 0 for i in range(65536))


def program(template, loops, count, dest, pointer):
    start = template.index(";; LOOP-BEGIN") + len(";; LOOP-BEGIN")
    end = template.index(";; LOOP-END")
    body = template[start:end]
    template = template[:start] + "".join(body.replace("$copy", f"$copy{index}") for index in range(loops)) + template[end:]
    image, source, checksum = bytearray(IMAGE), 8192, 0
    target = dest
    touched = {pointer, dest - 1, dest + loops * 256, 65535}
    for _ in range(loops):
        last = 0
        for index in range(count):
            last = image[pointer]
            image[target + index] = image[source + index + last]
            touched.update((target + index, source + index + last))
        checksum += last
        target += 256
        source += 32
    statements = [f"(if (i32.ne (call $copy_many (i32.const {dest}) (i32.const 8192) (i32.const {count}) (i32.const {pointer})) "
                  f"(i32.const {checksum})) (then unreachable))"]
    for address in sorted(touched):
        statements.append(f"(if (i32.ne (i32.load8_u (i32.const {address})) (i32.const {image[address]})) (then unreachable))")
    data = "".join(f"\\{byte:02x}" for byte in IMAGE[4096:])
    return template.rstrip()[:-1] + f'\n(data (i32.const 4096) "{data}")\n(func (export "_start")\n' + "\n".join(statements) + "\n))\n"


def main():
    vm, native, wazero, wat2wasm, fixture = sys.argv[1:]
    template = Path(fixture).read_text()
    cases = [(loops, count, dest, pointer) for loops in (4, 5, 7) for count in (0, 1, 17, 97)
             for dest, pointer in ((32768, 4096), (32768, 32768), (32768, 32833), (8191, 4096), (8193, 8192))]
    with tempfile.TemporaryDirectory(prefix="pizdos-wat-loop-escapes-") as directory:
        work = Path(directory)
        source, module, compiled, embedded = (work / name for name in ("input.wat", "input.wasm", "native.o", "embedded.o"))
        for case in cases:
            source.write_text(program(template, *case))
            result = subprocess.run([wat2wasm, str(source), "-o", str(module)], capture_output=True, timeout=30)
            assert result.returncode == 0, result.stderr.decode()
            result = subprocess.run([wazero, "-interpreter", str(module)], capture_output=True, timeout=30)
            assert result.returncode == 0 and result.stdout == b"", (case, result)
            for strict in (False, True):
                env = dict(os.environ, W2X_MEMORY_TAIL="strict" if strict else "optimized", W2X_TAIL_PROFILE="")
                result = subprocess.run([native, *(["--strict"] if strict else [])], input=module.read_bytes(), capture_output=True, env=env, timeout=60)
                assert result.returncode == 0, (case, strict, result.stderr)
                compiled.write_bytes(result.stdout)
                result = subprocess.run([vm, "-o", str(embedded), str(module)], capture_output=True, env=env, timeout=60)
                assert result.returncode == 0 and embedded.read_bytes() == compiled.read_bytes(), (case, strict, result)
                for obj in (compiled, embedded):
                    result = subprocess.run([vm, "run", "--", str(obj)], capture_output=True, env=env, timeout=30)
                    assert (result.returncode, result.stdout, result.stderr) == (0, b"", b""), (case, strict, result)
    print(f"WAT loop escapes: {len(cases)} sequences of 4/5/7 indirect, aliasing and overlapping copy loops agree with a bytewise model, wazero and both AOTs")


if __name__ == "__main__":
    main()
