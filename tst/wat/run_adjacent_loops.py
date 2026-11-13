#!/usr/bin/env python3
"""Place a constant or variable copy loop immediately before a second loop."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def program(template):
    prefix, rest = template.split(";; BEGIN_FUNCTION")
    body, suffix = rest.split(";; END_FUNCTION")
    functions, checks, cases = [], [], 0
    first_image = bytes((index * 37 + 11) % 251 for index in range(8192, 8448))
    second_image = bytes((index * 37 + 11) % 251 for index in range(12288, 12416))
    for fixed in (False, True):
        name = "fixed" if fixed else "variable"
        limit = "(i32.const 4)" if fixed else "(local.get $first)"
        functions.append(body.replace("$copy", "$" + name).replace(";; FIRST_LIMIT", limit))
        for first in ((1,) if fixed else (1, 17, 97)):
            for second in (1, 17, 97):
                for tag in (0, 1):
                    size = 4 if fixed else first
                    # A bytewise model checks each destination and the gaps
                    # and neighbouring bytes that must retain their canaries.
                    expected = {at: 85 for at in (32767, 32768 + size, 32895, 32896 + size,
                                                  36863, 36864 + second)}
                    for index in range(size):
                        expected[32768 + index] = first_image[index] ^ (0 if tag else 1)
                        expected[32896 + index] = first_image[128 + index]
                    for index in range(second):
                        expected[36864 + index] = second_image[index]
                    checks.extend(("(memory.fill (i32.const 32767) (i32.const 85) (i32.const 258))",
                                   "(memory.fill (i32.const 36863) (i32.const 85) (i32.const 130))",
                                   f"(if (i32.ne (call ${name} (i32.const 32768) (i32.const 8192) "
                                   f"(i32.const 36864) (i32.const 12288) (i32.const {first}) "
                                   f"(i32.const {second}) (i32.const {tag})) (i32.const {size + second})) (then unreachable))"))
                    for at, byte in sorted(expected.items()):
                        checks.append(f"(if (i32.ne (i32.load8_u (i32.const {at})) (i32.const {byte})) (then unreachable))")
                    cases += 1
    segments = []
    for at, data in ((8192, first_image), (12288, second_image)):
        encoded = "".join(f"\\{byte:02x}" for byte in data)
        segments.append(f'(data (i32.const {at}) "{encoded}")')
    suffix = suffix.replace(";; DATA", "\n".join(segments))
    text = prefix + "\n".join(functions) + suffix.rstrip()[:-1]
    return text + '\n(func (export "_start")\n' + "\n".join(checks) + "\n))\n", cases


def main():
    vm, native, wazero, wat2wasm, fixture = sys.argv[1:]
    text, count = program(Path(fixture).read_text())
    with tempfile.TemporaryDirectory(prefix="pizdos-wat-adjacent-loops-") as directory:
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
    print(f"WAT adjacent loops: {count} constant/variable bounds and branch cases agree with a bytewise model, wazero and both AOTs")


if __name__ == "__main__":
    main()
