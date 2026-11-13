#!/usr/bin/env python3
"""Reject malformed control flow, segments and unsupported import addresses.

    test_compiler_rejections.py <vm> <native-aot> <wazero>
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "dev"))

from exe import pack  # noqa: E402


def leb(value):
    result = bytearray()
    while value >= 128:
        result.append(value & 127 | 128)
        value >>= 7
    return bytes(result + bytes([value]))


def text(value):
    data = value.encode()
    return leb(len(data)) + data


def section(kind, body):
    return bytes([kind]) + leb(len(body)) + body


def program(code, *, elements=None, data=None, exports=None, import_name=None, addressable=False, global_import=False, globals=None, start=None, memory_import=False):
    types = b"\2\x60\0\0" + (b"\x60\1\x7f\1\x7f" if import_name == "__kernel_strlen" else b"\x60\0\1\x7f")
    module = b"\0asm\1\0\0\0" + section(1, types)
    if import_name:
        module += section(2, b"\1" + text("kernel") + text(import_name) + b"\0\1")
    elif global_import:
        module += section(2, b"\1" + text("env") + text("base") + b"\3\x7f\0")
    elif memory_import:
        module += section(2, b"\1" + text("env") + text("memory") + b"\2\0\1")
    module += section(3, b"\1\0") + section(4, b"\1\x70\0\1")
    if not memory_import:
        module += section(5, b"\1\0\1")
    if globals is not None:
        module += section(6, globals)
    exports = exports if exports is not None else text("_start") + b"\0" + leb(bool(import_name))
    if addressable:
        exports += text("alias") + b"\0\0"
    module += section(7, leb(2 if addressable else 1) + exports)
    if start is not None:
        module += section(8, leb(start))
    if elements is not None:
        module += section(9, elements)
    body = b"\0" + code + b"\x0b"
    module += section(10, b"\1" + leb(len(body)) + body)
    if data is not None:
        module += section(11, data)
    return module


def main():
    vm, native, wazero = sys.argv[1:]
    cases = [("large-branch-table", program(b"\x41\0\x0e" + leb(1048577)), "br_table too large"),
             ("branch-table-arity", program(b"\x02\x7f\x02\x40\x41\x2a\x41\0\x0e\1\0\1\x0b\x0b\x1a"), "type mismatch: br_table arity"),
             ("trailing-operators", program(b"\x0b\1"), "operators remaining after end of function"),
             ("else-without-if", program(b"\5"), "else without if"),
             ("unknown-block-type", program(b"\x02\2\x0b"), "unknown type"),
             ("unknown-table", program(b"\x41\0\x25\1\x1a"), "unknown table"),
             ("unknown-memarg-memory", program(b"\x41\0\x28\x42\1\0\x1a"), "unknown memory"),
             ("bad-memarg-flags", program(b"\x41\0\x28\x20\0\x1a"), "malformed memop flags"),
             ("unknown-table-init-segment", program(b"\x41\0" * 3 + b"\xfc\14\0\0"), "unknown elem segment"),
             ("unknown-element-drop", program(b"\xfc\15\0"), "unknown elem segment"),
             ("unknown-bulk-operator", program(b"\xfc\22"), "illegal opcode 0xfc"),
             ("unknown-data-segment", program(b"\xfc\11\1", data=b"\1\1\1x"), "unknown data segment"),
             ("bad-element-kind", program(b"", elements=b"\1\1\1\0"), "malformed element kind"),
             ("bad-elements-mode", program(b"", elements=b"\1\10"), "malformed elements segment kind"),
             ("bad-data-mode", program(b"", data=b"\1\3"), "malformed data segment kind"),
             ("bad-simd-constant", program(b"", globals=b"\1\x7b\0\xfd\x0d" + b"\0" * 16 + b"\x0b"), "constant expression required at offset"),
             ("bad-global-mutability", program(b"", globals=b"\1\x7f\2\x41\0\x0b"), "malformed mutability"),
             ("computed-element-offset", program(b"", global_import=True, elements=b"\1\0\x23\0\x0b\1\0"), "unsupported: element segment"),
             ("low-data", program(b"", data=b"\1\0\x41\0\x0b\1x"), "unsupported: data or the stack in the first KiB"),
             ("low-stack", program(b"", globals=b"\1\x7f\1\x41\x10\x0b"), "unsupported: data or the stack in the first KiB"),
             ("imported-memory-mode", program(b"", memory_import=True), "unsupported: data or the stack in the first KiB"),
             ("unknown-start", program(b"", start=33), "unknown function"),
             ("nonvoid-start", program(b"", import_name="getpid", start=0), "type mismatch"),
             ("bad-export-kind", program(b"", exports=text("_start") + b"\4\0"), "malformed export kind"),
             ("duplicate-export", program(b"", exports=text("alias") + b"\0\0", addressable=True), "duplicate export name"),
             ("undeclared-import-reference", program(b"\x41\0\xd2\0\x26\0", import_name="getpid"), "ref.func of a function no element, export or global names"),
             ("undeclared-intrinsic-reference", program(b"\x41\0\xd2\0\x26\0", import_name="__kernel_strlen"), "unsupported: the address of an import compiled at its calls"),
             ("intrinsic-import-address", program(b"\xd2\0\x1a", import_name="__kernel_strlen", addressable=True), "unsupported: the address of an import compiled at its calls")]
    env = dict(os.environ, W2X_MEMORY_TAIL="strict", W2X_TAIL_PROFILE="")
    with tempfile.TemporaryDirectory(prefix="pizdos-compiler-rejections-") as directory:
        work = Path(directory)
        source, output = work / "input.wasm", work / "output.o"
        for label, data, diagnostic in cases:
            low_memory = label in ("low-data", "low-stack")
            padded = low_memory or label in ("imported-memory-mode", "computed-element-offset")
            env = dict(env, W2X_MEMORY_TAIL="optimized" if padded else "strict")
            source.write_bytes(data)
            reference = subprocess.run([wazero, "-compile", str(source)], capture_output=True, timeout=30)
            if label not in ("intrinsic-import-address", "computed-element-offset", "low-data", "low-stack", "imported-memory-mode"):
                assert reference.returncode != 0, (label, reference)
            else:
                assert reference.returncode == 0, (label, reference)
            result = subprocess.run([native, *([] if padded else ["--strict"])], input=data, capture_output=True, env=env, timeout=30)
            assert result.returncode != 0 and diagnostic.encode() in result.stderr, (label, result.returncode, result.stderr)
            output.write_bytes(b"previous object")
            result = subprocess.run([vm, "-o", str(output), str(source)], capture_output=True, env=env, timeout=30)
            assert result.returncode == 1 and diagnostic.encode() in result.stderr and b"panic:" not in result.stderr, (label, result)
            assert output.read_bytes() == b"previous object" and not list(work.glob(".vm-*")), label
            if low_memory:
                strict_env = dict(env, W2X_MEMORY_TAIL="strict")
                result = subprocess.run([native, "--strict"], input=data, capture_output=True, env=strict_env, timeout=30)
                assert result.returncode == 0, (label, result)
                reference = result.stdout
                compiled = work / "native.o"
                compiled.write_bytes(reference)
                result = subprocess.run([vm, "-o", str(output), str(source)], capture_output=True, env=strict_env, timeout=30)
                assert result.returncode == 0 and output.read_bytes() == reference, (label, result)
                executable = work / "input"
                executable.write_bytes(pack(data))
                result = subprocess.run([vm, "run", "--", str(executable)], capture_output=True, env=strict_env, timeout=30)
                assert (result.returncode, result.stdout, result.stderr) == (0, b"", b""), (label, result)
    print(f"compiler rejections: {len(cases)} control, segment, index and import-address errors preserve output with both AOTs")


if __name__ == "__main__":
    main()
