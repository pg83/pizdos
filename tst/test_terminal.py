#!/usr/bin/env python3
"""Drive the public VM through a controlling pseudoterminal, checking echo.

    test_terminal.py <vm> <native-aot> <check.wasm> <filter>
"""
import errno
import fcntl
import os
from pathlib import Path
import pty
import select
import subprocess
import sys
import tempfile
import termios
import time


def terminal(command, answers, expected, env):
    master, slave = pty.openpty()

    def acquire():
        os.setsid()
        fcntl.ioctl(slave, termios.TIOCSCTTY, 0)

    child = subprocess.Popen(command, stdin=slave, stdout=slave, stderr=slave, env=env, preexec_fn=acquire)
    os.close(slave)
    output = bytearray()
    deadline, answered = time.monotonic() + 15, 0
    try:
        while True:
            remaining = deadline - time.monotonic()
            assert remaining > 0, ("terminal timeout", bytes(output))
            ready, _, _ = select.select([master], [], [], remaining)
            assert ready, ("terminal timeout", bytes(output))
            try:
                data = os.read(master, 4096)
            except OSError as error:
                if error.errno != errno.EIO:
                    raise
                break
            if not data:
                break
            output.extend(data)
            if answered < len(answers) and output.endswith(answers[answered][0]):
                os.write(master, answers[answered][1])
                answered += 1
        assert child.wait(timeout=5) == 0, bytes(output)
        assert answered == len(answers) and bytes(output) == expected, bytes(output)
    finally:
        if child.poll() is None:
            child.kill()
        child.wait()
        os.close(master)


def main():
    vm, native, module, wrapper = sys.argv[1:]
    env = dict(os.environ, W2X_MEMORY_TAIL="optimized", W2X_TAIL_PROFILE="")
    with tempfile.TemporaryDirectory(prefix="pizdos-terminal-") as directory:
        root = Path(directory)
        (root / "bin").mkdir()
        compiled, embedded = root / "bin/check", root / "bin/embedded"
        result = subprocess.run([native], input=Path(module).read_bytes(), capture_output=True, env=env, timeout=60)
        assert result.returncode == 0, result.stderr
        compiled.write_bytes(result.stdout)
        subprocess.run([vm, "-o", str(embedded), module], capture_output=True, env=env, check=True, timeout=60)
        assert compiled.read_bytes() == embedded.read_bytes()
        for obj in (compiled, embedded):
            command = [vm, "run", "--root", str(root), "--", str(obj)]
            result = subprocess.run([*command, "missing"], start_new_session=True, capture_output=True, env=env, timeout=30)
            assert (result.returncode, result.stdout, result.stderr) == (0, b"missing terminal: ok\n", b""), result
            terminal([*command, "password"], [(b"first: ", b"s3cr3t\n"), (b"empty: ", b"\n"),
                                              (b"long: ", b"x" * 150 + b"\n"), (b"eof: ", b"\x04")],
                     b"first: \r\nempty: \r\nlong: \r\neof: \r\nterminal passwords: ok\r\n", env)
            terminal([*command, "interrupt"], [], b"interrupt: interrupted terminal: ok\r\n", env)
            terminal([wrapper, f"{errno.ENOTTY}/16", "--", *command, "attributes"], [],
                     b"terminal attributes: failed as expected\r\n", env)
    print("terminal: native/Wasm AOT agree, passwords remain unechoed, attributes restored and errors reported")


if __name__ == "__main__":
    main()
