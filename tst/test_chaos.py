#!/usr/bin/env python3
"""Faults exercise real AOT/VM commands; production executables ignore them.

    test_chaos.py <vm> <vm-chaos> <native-aot> <native-aot-chaos>
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

EMPTY = b"\0asm\1\0\0\0"


def call(command, *, data=None, **variables):
    environment = dict(os.environ)
    environment.pop("PIZDOS_AOT_CHAOS", None)
    environment.pop("PIZDOS_VM_CHAOS", None)
    environment.update(variables)
    return subprocess.run(command, input=data, capture_output=True, env=environment, timeout=60)


def main():
    vm, vm_chaos, native, native_chaos = sys.argv[1:]
    reference = call([native], data=EMPTY)
    assert reference.returncode == 0, reference.stderr
    assert call([native_chaos], data=EMPTY).stdout == reference.stdout
    with tempfile.TemporaryDirectory(prefix="pizdos-chaos-") as directory:
        work = Path(directory)
        source, destination = work / "input.wasm", work / "out.o"
        source.write_bytes(EMPTY)

        def compile_with(binary, **variables):
            destination.write_bytes(b"previous object")
            result = call([binary, "-o", str(destination), str(source)], **variables)
            if result.returncode:
                assert destination.read_bytes() == b"previous object", "failed compiler replaced the output"
                assert not list(work.glob(".vm-*")), "failed compiler leaked temporary output"
            else:
                assert destination.read_bytes() == reference.stdout, "compiler output changed"
            return result

        assert compile_with(vm, PIZDOS_AOT_CHAOS="read@0", PIZDOS_VM_CHAOS="invalid").returncode == 0
        plain = call([native], data=EMPTY, PIZDOS_AOT_CHAOS="read@0")
        assert plain.returncode == 0 and plain.stdout == reference.stdout
        assert compile_with(vm_chaos).returncode == 0

        for rule, native_text, wasm_text in (
            ("read@0", b"stdin read failed", b"stdin read failed"),
            ("read@1", b"stdin read failed", b"stdin read failed"),
            ("write@0", b"", b"aot exited with code 1"),
            ("write@1", b"", b"aot exited with code 1"),
            ("grow@0", b"allocator mapping failed", b"out of memory"),
        ):
            result = call([native_chaos], data=EMPTY, PIZDOS_AOT_CHAOS=rule)
            assert result.returncode != 0 and native_text in result.stderr, (rule, result)
            result = compile_with(vm_chaos, PIZDOS_AOT_CHAOS=rule)
            assert result.returncode != 0 and wasm_text in result.stderr, (rule, result)
            assert compile_with(vm_chaos).returncode == 0, "a later compilation did not recover"

        for rule, diagnostic in (("mmap@0", b"cannot allocate memory"),
                                 ("compile-read@0", b"unexpected EOF"),
                                 ("compile-write@0", b"short write")):
            result = compile_with(vm_chaos, PIZDOS_VM_CHAOS=rule)
            assert result.returncode != 0 and diagnostic in result.stderr, (rule, result)
            assert compile_with(vm_chaos).returncode == 0

        for script in ("missing@0", "read@-1", "read@", "read@1,", "read@0,read@1", "write@4294967296"):
            result = call([native_chaos], data=EMPTY, PIZDOS_AOT_CHAOS=script)
            assert result.returncode != 0 and b"invalid PIZDOS_AOT_CHAOS" in result.stderr, (script, result)
            result = compile_with(vm_chaos, PIZDOS_AOT_CHAOS=script)
            assert result.returncode != 0 and b"invalid chaos rule" in result.stderr, (script, result)
        assert compile_with(vm_chaos, PIZDOS_AOT_CHAOS="read@4294967295,write@4294967295,grow@4294967295").returncode == 0
        result = compile_with(vm_chaos, PIZDOS_VM_CHAOS="missing@0")
        assert result.returncode != 0 and b"invalid chaos rule" in result.stderr

    print("chaos: native/Wasm AOT and VM faults, recovery, atomic output and production no-ops passed")


if __name__ == "__main__":
    main()
