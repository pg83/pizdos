#!/usr/bin/env python3
"""Check explicit copy addresses and loop-carried integer values."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

IMAGE = bytes((i * 37 + 11) % 251 if i >= 4096 else 0 for i in range(65536))


def address(base, offset, form):
    index = f"(i32.add (local.get $index) (i32.const {offset}))"
    if form == "spaced":
        index = f"(i32.add (local.get $index) (i32.const {offset * 7}))"
    if form == "subtract":
        return f"(i32.sub (i32.add (local.get ${base}) {index}) (local.get $delta))"
    if form == "hoisted":
        return f"(i32.add (i32.sub (local.get ${base}) (local.get $delta)) {index})"
    if form == "cancel":
        return f"(i32.sub (i32.add (i32.add (local.get ${base}) {index}) (local.get $delta)) (local.get $delta))"
    return f"(i32.add (local.get ${base}) {index})"


def program(template, stride, form, carry, count, dest, other, variable_step=False, mixed_offset=False):
    statements = []
    for offset in range(stride):
        base = "other" if form == "distinct" and offset else "source"
        load = f"(i32.load8_u {address(base, offset, form)})"
        if carry == "last":
            statements.append(f"(local.set $state {load})")
            load = "(local.get $state)"
        statements.append(f"(i32.store8 {address('dest', offset, form)} {load})")
    expression = "(local.get $index)"
    for salt in range(1, 31):
        expression = f"(i32.xor (i32.mul {expression} (i32.const 33)) (i32.const {salt}))"
    update = {"none": "", "wide": "(local.set $sum (i64.add (local.get $sum) (i64.const 3)))",
              "size": "(local.set $state (memory.size))",
              "last": "", "constant": "(local.set $state (i32.const 42))",
              "deep": f"(local.set $state {expression})",
              "tail_read": "(local.set $state (i32.load8_u (i32.add (local.get $source) (local.get $index))))",
              "nonlinear": "(local.set $state (i32.xor (i32.mul (local.get $state) (i32.const 33)) (local.get $index)))"}[carry]
    step = f"(i32.add (local.get $delta) (i32.const {stride}))" if variable_step else f"(i32.const {stride})"
    template = template.replace(";; COPY", "\n".join(statements)).replace(";; CARRY", update).replace("(i32.const STRIDE)", step)
    offset_update = "(local.set $offset (i32.add (i32.add (local.get $offset) (i32.const 3)) (local.get $delta)))" if mixed_offset else ""
    if mixed_offset:
        template = template.replace("(local.get $index)", "(local.get $offset)", 1)
    template = template.replace(";; OFFSET", offset_update)
    image, checksum, state, delta = bytearray(IMAGE), 0, 7, 13
    touched = {dest - 32, dest + count + 32, 65535}
    moving_offset = 0
    for index in range(0, count, stride + (delta if variable_step else 0)):
        for offset in range(stride):
            source = other if form == "distinct" and offset else 8192
            shift = delta if form in ("subtract", "hoisted") else 0
            spacing = offset * 7 if form == "spaced" else offset
            write_index = moving_offset if mixed_offset and offset == 0 else index
            read, write = source + index + spacing - shift, dest + write_index + spacing - shift
            loaded = image[read]
            image[write] = loaded
            if carry == "last":
                state = loaded
            touched.update((read, write))
        if carry == "wide":
            checksum += 3
        elif carry == "nonlinear":
            state = ((state * 33) ^ index) & 0xffffffff
        elif carry == "size":
            state = 1
        elif carry == "constant":
            state = 42
        elif carry == "deep":
            state = index
            for salt in range(1, 31):
                state = ((state * 33) ^ salt) & 0xffffffff
        elif carry == "tail_read":
            state = image[8192 + index]
            touched.add(8192 + index)
        moving_offset += 3 + delta
    expected = checksum + state
    checks = [f"(if (i64.ne (call $copy (i32.const {dest}) (i32.const 8192) (i32.const {other}) "
              f"(i32.const {count}) (i32.const {delta})) (i64.const {expected})) (then unreachable))"]
    for location in sorted(touched):
        checks.append(f"(if (i32.ne (i32.load8_u (i32.const {location})) (i32.const {image[location]})) (then unreachable))")
    data = "".join(f"\\{byte:02x}" for byte in IMAGE[4096:])
    return template.rstrip()[:-1] + f'\n(data (i32.const 4096) "{data}")\n(func (export "_start")\n' + "\n".join(checks) + "\n))\n"


def main():
    vm, native, wazero, wat2wasm, fixture = sys.argv[1:]
    template = Path(fixture).read_text()
    cases = [(stride, form, carry, count, dest, other) for stride in (1, 2, 3)
             for form in ("add", "subtract", "cancel", "distinct") for carry in ("none", "wide", "nonlinear")
             for count, dest, other in ((0, 32768, 8192), (1, 32768, 12288), (97, 32768, 12288), (97, 8193, 8192))]
    cases += [(stride, "add", carry, count, dest, 8192, True) for stride in (1, 3) for carry in ("none", "wide")
              for count, dest in ((0, 32768), (1, 32768), (97, 8193))]
    cases += [(stride, form, carry, count, dest, 8192) for stride in (1, 2, 3)
              for form, carry in (("hoisted", "none"), ("add", "size"), ("add", "tail_read"),
                                  ("add", "last"), ("add", "constant"), ("add", "deep"))
              for count, dest in ((0, 32768), (1, 32768), (97, 32768), (97, 8193))]
    cases += [(stride, "spaced", "none", count, 32768, 8192) for stride in (2, 3) for count in (0, 1, 97)]
    cases += [(stride, "add", "none", count, 32768, 8192, False, True) for stride in (1, 2, 3) for count in (0, 1, 97)]
    with tempfile.TemporaryDirectory(prefix="pizdos-wat-copy-addresses-") as directory:
        work = Path(directory)
        source, module, compiled, embedded = (work / name for name in ("input.wat", "input.wasm", "native.o", "embedded.o"))
        for case in cases:
            source.write_text(program(template, *case))
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
                for obj in (compiled, embedded):
                    result = subprocess.run([vm, "run", "--", str(obj)], capture_output=True, env=env, timeout=30)
                    assert (result.returncode, result.stdout, result.stderr) == (0, b"", b""), (case, strict, result)
    print(f"WAT copy addresses: {len(cases)} direct/subtractive/cancelled/distinct addresses and i32/i64 loop states agree with a bytewise model, wazero and both AOTs")


if __name__ == "__main__":
    main()
