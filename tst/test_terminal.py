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
import struct
import subprocess
import sys
import tempfile
import termios
import time


def private_name(vm, root, objects):
    devices = Path(root) / "ttys"
    devices.mkdir()
    subprocess.run(["mount", "-t", "devpts", "-o", "newinstance,ptmxmode=666,mode=620", "devpts", str(devices)], check=True)
    master = os.open(devices / "ptmx", os.O_RDWR | os.O_NOCTTY)
    try:
        fcntl.ioctl(master, 0x40045431, struct.pack("i", 0))  # TIOCSPTLCK.
        number = struct.unpack("I", fcntl.ioctl(master, 0x80045430, bytes(4)))[0]  # TIOCGPTN.
        slave = os.open(devices / str(number), os.O_RDWR | os.O_NOCTTY)
        try:
            for obj in objects:
                result = subprocess.run([vm, "run", "--root", root, "--", obj, "private-name"], stdin=slave,
                                        capture_output=True, timeout=30)
                assert (result.returncode, result.stdout, result.stderr) == (0, b"private terminal name: ok\n", b""), result
        finally:
            os.close(slave)
    finally:
        os.close(master)
        subprocess.run(["umount", str(devices)], check=True)
    print("private terminal: both AOTs resolve and reopen the guest path")


def heapless_password():
    # Reserve the static table's page and the heap segment's 31 other pages.
    # getpass then needs another size class and must report ENOMEM after input.
    def section(kind, body):
        assert len(body) < 128
        return bytes([kind, len(body)]) + body

    data = b"\0asm\1\0\0\0" + section(1, b"\2\x60\1\x7f\1\x7f\x60\0\0")
    data += section(2, b"\3\6kernel\7getpass\0\0\6kernel\14setusershell\0\1\6kernel\6malloc\0\0") + section(3, b"\1\1")
    data += section(5, b"\1\1\1\x21") + section(6, b"\1\x7f\0\x41\x80\x80\4\x0b")
    data += section(7, b"\2\6_start\0\3\13__heap_base\3\0")
    code = b"\0\x10\1\x41\x1f\x41\x10\x74\x10\2\x45\x04\x40\0\x0b"
    code += b"\x41\x80\x20\x10\0\x04\x40\0\x0b"
    code += b"\x41\0\x28\2\0\x41\14\x47\x04\x40\0\x0b\x0b"
    data += section(10, b"\1" + bytes([len(code)]) + code)
    return data + section(11, b"\1\0\x41\x80\x20\x0b\6oom: \0")


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
    if sys.argv[1] == "--private-name":
        private_name(sys.argv[2], sys.argv[3], sys.argv[4:])
        return
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
        # A devpts instance mounted inside an isolated user/mount namespace
        # gives a real terminal a path in the guest root, outside /dev/pts.
        result = subprocess.run(["unshare", "--user", "--map-root-user", "--mount", "--propagation", "private",
                                 sys.executable, __file__, "--private-name", vm, str(root), str(compiled), str(embedded)],
                                capture_output=True, env=env, timeout=60)
        assert (result.returncode, result.stdout, result.stderr) == (0, b"private terminal: both AOTs resolve and reopen the guest path\n", b""), result
        source = root / "heapless.wasm"
        source.write_bytes(heapless_password())
        result = subprocess.run([native], input=source.read_bytes(), capture_output=True, env=env, timeout=60)
        assert result.returncode == 0, result.stderr
        compiled.write_bytes(result.stdout)
        result = subprocess.run([vm, "-o", str(embedded), str(source)], capture_output=True, env=env, timeout=60)
        assert result.returncode == 0 and embedded.read_bytes() == compiled.read_bytes(), result
        for obj in (compiled, embedded):
            terminal([vm, "run", "--root", str(root), "--", str(obj)], [(b"oom: ", b"\n")], b"oom: \r\n", env)
    print("terminal: native/Wasm AOT agree, passwords remain unechoed, attributes restored and errors reported")


if __name__ == "__main__":
    main()
