#!/usr/bin/env python3
"""Keep long address sums explicit, including argument vectors across pages."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def program(template, parameters):
    address = "local.get 0\n" + "".join(f"local.get {index}\ni32.add\n" for index in range(1, parameters))
    template = template.replace(";; PARAMETERS", "(param " + " ".join(["i32"] * parameters) + ")")
    template = template.replace(";; ADDRESS", address)
    image = bytes((index * 37 + 11) % 251 for index in range(4096, 65536))
    checks = []
    samples = [[4096] + [1] * (parameters - 1), [8192] + [index % 3 for index in range(1, parameters)],
               [4096] + [0] * (parameters - 1)]
    for values in samples:
        expected = image[sum(values) - 4096]
        call = "(call $load " + " ".join(f"(i32.const {value})" for value in values) + ")"
        checks.append(f"(if (i32.ne {call} (i32.const {expected})) (then unreachable))")
    data = "".join(f"\\{byte:02x}" for byte in image)
    return template.rstrip()[:-1] + f'\n(data (i32.const 4096) "{data}")\n(func (export "_start")\n' + "\n".join(checks) + "\n))\n"


def main():
    vm, native, wazero, wat2wasm, fixture = sys.argv[1:]
    template = Path(fixture).read_text()
    with tempfile.TemporaryDirectory(prefix="pizdos-wat-deep-addresses-") as directory:
        work = Path(directory)
        source, module, compiled, embedded = (work / name for name in ("input.wat", "input.wasm", "native.o", "embedded.o"))
        for parameters in (41, 257, 16385):
            source.write_text(program(template, parameters))
            result = subprocess.run([wat2wasm, str(source), "-o", str(module)], capture_output=True, timeout=30)
            assert result.returncode == 0, (parameters, result.stderr.decode())
            result = subprocess.run([wazero, "-interpreter", str(module)], capture_output=True, timeout=60)
            assert result.returncode == 0 and result.stdout == b"", (parameters, result)
            for strict in (False, True):
                env = dict(os.environ, W2X_MEMORY_TAIL="strict" if strict else "optimized", W2X_TAIL_PROFILE="")
                result = subprocess.run([native, *(["--strict"] if strict else [])], input=module.read_bytes(), capture_output=True, env=env, timeout=120)
                assert result.returncode == 0, (parameters, strict, result.stderr)
                compiled.write_bytes(result.stdout)
                result = subprocess.run([vm, "-o", str(embedded), str(module)], capture_output=True, env=env, timeout=120)
                assert result.returncode == 0 and embedded.read_bytes() == compiled.read_bytes(), (parameters, strict, result)
                for obj in (compiled, embedded):
                    result = subprocess.run([vm, "run", "--", str(obj)], capture_output=True, env=env, timeout=30)
                    assert (result.returncode, result.stdout, result.stderr) == (0, b"", b""), (parameters, strict, result)
    print("WAT deep addresses: sums of 41/257/16385 arguments and repeated large argument vectors agree with a byte model, wazero and both AOTs")


if __name__ == "__main__":
    main()
