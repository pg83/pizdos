#!/usr/bin/env python3
"""The kernel boots: it starts rootfs.wasm under wazero, reads /bin/aot and
/bin/rootfs from it and compiles both with aot to the host tool's bytes.

    test_kernel.py <kernel> <rootfs.aot> <aot.aot>
"""
import hashlib
import re
import subprocess
import sys
from pathlib import Path


def main():
    kernel, rootfs_aot, aot_aot = sys.argv[1:4]
    result = subprocess.run([kernel], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    compiled = dict(re.findall(r"^kernel: compiled (\S+): .* sha256 ([0-9a-f]{64}) ", result.stdout, re.M))
    expected = {
        "/bin/rootfs": hashlib.sha256(Path(rootfs_aot).read_bytes()).hexdigest(),
        "/bin/aot": hashlib.sha256(Path(aot_aot).read_bytes()).hexdigest(),
    }
    assert compiled == expected, (result.stdout, expected)
    assert result.stdout.rstrip().endswith("kernel: wireboard ready"), result.stdout


if __name__ == "__main__":
    main()
