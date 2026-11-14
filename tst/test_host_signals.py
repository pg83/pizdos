#!/usr/bin/env python3
"""Drive signal delivery and job control through the actual VM process.

    test_host_signals.py <vm> <native-aot> <wazero>
"""
import os
from pathlib import Path
import select
import signal
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "dev"))

from exe import pack  # noqa: E402


def leb(value, signed=False):
    result = bytearray()
    while True:
        byte = value & 127
        value >>= 7
        if value == 0 and (not signed or byte < 64) or signed and value == -1 and byte >= 64:
            return bytes(result + bytes([byte]))
        result.append(byte | 128)


def text(value):
    data = value.encode()
    return leb(len(data)) + data


def section(kind, body):
    return bytes([kind]) + leb(len(body)) + body


def constant(value):
    return b"\x41" + leb(value, True)


def check(value, expected):
    return value + constant(expected) + b"\x47\x04\x40\0\x0b"


def program(mode):
    # sigprocmask and sigwaitinfo take the size of each structure after its
    # pointer: 8 for a sigset_t, 36 for a siginfo_t.
    imports = [("write", 1), ("raise", 2), ("sigprocmask", 5), ("sigwaitinfo", 6), ("signal", 3)]
    types = [b"\x60\0\0", b"\x60\3\x7f\x7f\x7f\1\x7f", b"\x60\1\x7f\1\x7f",
             b"\x60\2\x7f\x7f\1\x7f", b"\x60\2\x7f\x7f\0",
             b"\x60\5" + b"\x7f" * 5 + b"\1\x7f", b"\x60\4" + b"\x7f" * 4 + b"\1\x7f"]
    def write(at):
        return check(constant(1) + constant(at) + constant(1) + b"\x10\0", 1)

    entry = b""
    if mode == "wait":
        entry += check(constant(0) + constant(4096) + constant(8) + constant(0) + constant(8) + b"\x10\2", 0)
    elif mode in ("handler", "missing-stack"):
        entry += check(constant(10) + constant(1) + b"\x10\4", 0)
    entry += write(4112)
    if mode == "loop":
        entry += b"\x03\x40\x0c\0\x0b"
    elif mode == "stop":
        entry += check(constant(19) + b"\x10\1", 0) + write(4113)
    elif mode == "wait":
        entry += check(constant(4096) + constant(8) + constant(0) + constant(36) + b"\x10\3", 10) + write(4113)
    else:
        # The handler changes an ordinary mutable global, then returns to
        # this uninterrupted loop's condition.
        entry += b"\x03\x40\x23\0\x45\x0d\0\x0b" + write(4113)
    callback = check(b"\x20\0", 1) + check(b"\x20\1", 10) + write(4114) + constant(1) + b"\x24\0"
    module = b"\0asm\1\0\0\0" + section(1, leb(len(types)) + b"".join(types))
    module += section(2, leb(len(imports)) + b"".join(text("kernel") + text(name) + b"\0" + leb(typ) for name, typ in imports))
    module += section(3, b"\2\0\4") + section(5, b"\1\1\1\1")
    globals = b"\x7f\1" + constant(0) + b"\x0b"
    if mode == "handler":
        globals += b"\x7f\1" + constant(32768) + b"\x0b"
    module += section(6, bytes([2 if mode == "handler" else 1]) + globals)
    exports = text("_start") + b"\0\5" + text("__call_signal_handler") + b"\0\6"
    if mode == "handler":
        exports += text("__stack_pointer") + b"\3\1"
    module += section(7, bytes([3 if mode == "handler" else 2]) + exports)
    bodies = [b"\0" + body + b"\x0b" for body in (entry, callback)]
    module += section(10, b"\2" + b"".join(leb(len(body)) + body for body in bodies))
    image = (1 << 9).to_bytes(8, "little") + bytes(8) + b"RCH"
    return module + section(11, b"\1\0" + constant(4096) + b"\x0b" + leb(len(image)) + image)


def wait_ready(child):
    ready, _, _ = select.select([child.stdout], [], [], 15)
    assert ready and os.read(child.stdout.fileno(), 1) == b"R", "guest did not become ready"


def wait_stopped(child):
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        pid, status = os.waitpid(child.pid, os.WUNTRACED | os.WNOHANG)
        if pid:
            assert os.WIFSTOPPED(status) and os.WSTOPSIG(status) == signal.SIGSTOP, status
            return
        time.sleep(0.005)
    raise AssertionError("VM did not stop for its first guest")


def main():
    vm, native, wazero = sys.argv[1:]
    env = dict(os.environ, W2X_MEMORY_TAIL="strict", W2X_TAIL_PROFILE="")
    checked = 0
    with tempfile.TemporaryDirectory(prefix="pizdos-host-signals-") as directory:
        work = Path(directory)
        source, executable, compiled, embedded = [work / name for name in ("input.wasm", "input", "native.o", "embedded.o")]
        for mode in ("loop", "stop", "wait", "handler", "missing-stack"):
            data = program(mode)
            source.write_bytes(data)
            executable.write_bytes(pack(data))
            result = subprocess.run([wazero, "-compile", str(source)], capture_output=True, timeout=30)
            assert result.returncode == 0, (mode, result)
            result = subprocess.run([native, "--strict"], input=data, capture_output=True, env=env, timeout=60)
            assert result.returncode == 0, (mode, result.stderr)
            compiled.write_bytes(result.stdout)
            result = subprocess.run([vm, "-o", str(embedded), str(source)], capture_output=True, env=env, timeout=60)
            assert result.returncode == 0 and embedded.read_bytes() == compiled.read_bytes(), (mode, result)
            for sig in ((signal.SIGTERM, signal.SIGINT, signal.SIGUSR2) if mode == "loop" else (signal.SIGUSR1,)):
                child = subprocess.Popen([vm, "run", "--", str(executable)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
                try:
                    wait_ready(child)
                    if mode == "stop":
                        wait_stopped(child)
                        child.send_signal(signal.SIGCONT)
                    else:
                        child.send_signal(sig)
                    output, errors = child.communicate(timeout=15)
                    if mode == "missing-stack":
                        assert child.returncode == 134 and output == b"" and b"setjmp: no __stack_pointer" in errors and b"panic:" not in errors, (child.returncode, output, errors)
                        checked += 1
                        continue
                    expected = b"HC" if mode == "handler" else b"C" if mode != "loop" else b""
                    status = 128 + sig if mode == "loop" else 0
                    assert (child.returncode, output, errors) == (status, expected, b""), (mode, sig, child.returncode, output, errors)
                    checked += 1
                finally:
                    if child.poll() is None:
                        child.kill()
                    child.communicate()
    print(f"host signals: {checked} fatal/blocked/handled signals and guest stop/host continue exchanges passed with both AOTs")


if __name__ == "__main__":
    main()
