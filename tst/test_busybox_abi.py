#!/usr/bin/env python3
"""busybox.wasm talks to nothing but the kernel and exposes the entry points the kernel calls.

    test_busybox_abi.py <busybox.wasm> <busybox.links>
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "dev"))

import wasm_info  # noqa: E402

EXPORTS = {
    ("_start", "func"),
    ("__call_signal_handler", "func"),
    ("__call_signal_action", "func"),
    ("__heap_base", "global"),
    ("memory", "memory"),
}


def main():
    imports, exports = wasm_info.module_info(Path(sys.argv[1]).read_bytes())
    foreign = [entry for entry in imports if entry[0] != "kernel" or entry[2] != "func"]
    assert not foreign, f"imports outside the kernel's functions: {foreign}"
    assert ("kernel", "__kernel_start", "func") in imports, "the crt does not start through the kernel"
    missing = EXPORTS - set(exports)
    assert not missing, f"missing exports: {sorted(missing)}"
    links = Path(sys.argv[2]).read_text().split()
    assert len(links) == len(set(links)), "duplicate applet links"
    assert all(link.startswith("/") for link in links), links
    assert {"/bin/sh", "/bin/ls", "/usr/bin/env"} <= set(links), links


if __name__ == "__main__":
    main()
