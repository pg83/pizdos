#!/usr/bin/env python3
"""aot as a wasm module: it imports nothing, keeps the blob interface of
bin/aot/wasm and compiles to the same bytes as the host tool. aot.wasm runs
as aot compiled it (aot.aot), mapped through tst/aot/harness.py, and
compiles ops.wasm and then itself.

    test_aot_wasm.py <aot.wasm> <aot.aot> <ops.wasm> <ops.aot>
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "aot"))
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "dev"))

import wasm_info  # noqa: E402
from harness import Instance  # noqa: E402

EXPORTS = {
    ("memory", "memory"),
    ("aot_input", "func"),
    ("aot_compile", "func"),
    ("aot_output", "func"),
    ("aot_output_size", "func"),
}


def compile(instance, module):
    """The status and the output of aot_compile for module."""
    address = instance.call("aot_input", len(module)) & 0xFFFFFFFF
    instance.memory[address:address + len(module)] = module
    status = instance.call("aot_compile")
    output = instance.call("aot_output") & 0xFFFFFFFF
    size = instance.call("aot_output_size") & 0xFFFFFFFF

    return status, bytes(instance.memory[output:output + size])


def main():
    aot_wasm, aot_aot, ops_wasm, ops_aot = (Path(path).read_bytes() for path in sys.argv[1:5])
    imports, exports = wasm_info.module_info(aot_wasm)
    assert not imports, imports
    assert EXPORTS <= set(exports), sorted(EXPORTS - set(exports))
    instance = Instance(aot_wasm, aot_aot, {})

    status, output = compile(instance, ops_wasm)
    assert status == 0, output.decode(errors="replace")
    assert output == ops_aot, "aot.wasm and the host tool compiled ops.wasm differently"

    status, output = compile(instance, b"\0asm\1\0\0\0\x01")
    assert status == 1 and output, (status, output)

    status, output = compile(instance, aot_wasm)
    assert status == 0, output.decode(errors="replace")
    assert output == aot_aot, "aot.wasm compiled itself differently from the host tool"


if __name__ == "__main__":
    main()
