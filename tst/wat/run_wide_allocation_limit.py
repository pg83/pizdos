#!/usr/bin/env python3
"""Reject a wide-string copy beyond the heap limit while signals are pending."""
import os
from pathlib import Path
import select
import signal
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "dev"))

from exe import pack  # noqa: E402


def main():
    vm, native, wazero, wat2wasm, fixture = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="pizdos-wat-wide-limit-") as directory:
        work = Path(directory)
        module, executable, compiled, embedded = (work / name for name in ("input.wasm", "input", "native.o", "embedded.o"))
        result = subprocess.run([wat2wasm, fixture, "-o", str(module)], capture_output=True, timeout=30)
        assert result.returncode == 0, result.stderr.decode()
        result = subprocess.run([wazero, "-compile", str(module)], capture_output=True, timeout=30)
        assert result.returncode == 0, result
        for strict in (False, True):
            env = dict(os.environ, W2X_MEMORY_TAIL="strict" if strict else "optimized", W2X_TAIL_PROFILE="")
            result = subprocess.run([native, *(["--strict"] if strict else [])], input=module.read_bytes(), capture_output=True, env=env, timeout=60)
            assert result.returncode == 0, (strict, result.stderr)
            compiled.write_bytes(result.stdout)
            result = subprocess.run([vm, "-o", str(embedded), str(module)], capture_output=True, env=env, timeout=60)
            assert result.returncode == 0 and embedded.read_bytes() == compiled.read_bytes(), (strict, result)
            # Each VM releases the 2 GiB input before the next one starts.
            executable.write_bytes(pack(module.read_bytes()))
            child = subprocess.Popen([vm, "run", "--", str(executable)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
            try:
                ready, _, _ = select.select([child.stdout], [], [], 60)
                assert ready and os.read(child.stdout.fileno(), 1) == b"R", "guest did not finish filling the input"
                time.sleep(0.05)
                for _ in range(10):
                    if child.poll() is None:
                        child.send_signal(signal.SIGUSR1)
                    time.sleep(0.02)
                out, err = child.communicate(timeout=180)
                assert (child.returncode, out, err) == (0, b"", b""), (strict, child.returncode, out, err)
            finally:
                if child.poll() is None:
                    child.kill()
                child.communicate()
    print("WAT wide allocation limit: both AOTs reject a 2 GiB wide-string copy with ENOMEM and deliver signals after the kernel call in strict/optimized modes")


if __name__ == "__main__":
    main()
