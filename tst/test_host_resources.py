#!/usr/bin/env python3
"""Guest calls under a real host descriptor limit must fail and recover.

    test_host_resources.py <vm> <native-aot> <fixture.wasm>
"""
import os
from pathlib import Path
import resource
import subprocess
import sys
import tempfile


def limit_descriptors():
    resource.setrlimit(resource.RLIMIT_NOFILE, (64, 64))


def main():
    vm, native, fixture = sys.argv[1:]
    env = dict(os.environ, W2X_MEMORY_TAIL="strict", W2X_TAIL_PROFILE="")
    with tempfile.TemporaryDirectory(prefix="pizdos-host-resources-") as directory:
        work = Path(directory)
        compiled, embedded = work / "native.o", work / "embedded.o"
        result = subprocess.run([native, "--strict"], input=Path(fixture).read_bytes(), capture_output=True, env=env, timeout=60)
        assert result.returncode == 0, result
        compiled.write_bytes(result.stdout)
        result = subprocess.run([vm, "-o", str(embedded), fixture], capture_output=True, env=env, timeout=60)
        assert result.returncode == 0 and embedded.read_bytes() == compiled.read_bytes(), result
        for obj in (compiled, embedded):
            for operation in ("objects", "fork", "vfork", "forkpty"):
                result = subprocess.run([vm, "run", "--root", str(work), "--", str(obj), operation], capture_output=True,
                                        env=env, preexec_fn=limit_descriptors, timeout=30)
                assert (result.returncode, result.stdout, result.stderr) == (0, b"host resource recovery: ok\n", b""), (operation, result)
    print("host resources: pipes, sockets, PTYs and process creation recover from real descriptor exhaustion with both AOTs")


if __name__ == "__main__":
    main()
