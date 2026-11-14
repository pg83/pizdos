#!/usr/bin/env python3
"""Compile a C++ program, check that both AOT executables compile it to the
same object, and check its behaviour natively and as a module the kernel
compiles and runs.

    run.py <case.cpp> <output-prefix> <toolchain-json>

Directives use trustme's //@ spelling. stdout/stderr are JSON strings;
compile-flags, aot-flags, run-flags and args use shell quoting. host: false
disables the native reference for programs that depend on this kernel.
$ROOT in an env directive names the reference's working directory or the
VM's isolated root, so host services can read files the guest creates; in
run-flags, the VM's root, where bin/check is the program's executable.
"""
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tarfile
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "dev"))
import wasm
import exe

LINK = ["--stack-first", "-z", "stack-size=1049600", "--max-memory=4294967296",
        "--export=__heap_base", "--export=__stack_pointer", "--gc-sections"]
DIRECTIVES = {"compile-flags", "aot-flags", "run-flags", "args", "env", "stdout", "stderr", "stderr-pattern", "exit-code", "host", "timeout", "freestanding", "max-memory"}


def directives(source):
    result = {}

    for key, value in re.findall(r"^//@\s*([a-z-]+):\s*(.*?)\s*$", source.read_text(), re.M):
        if key not in DIRECTIVES:
            raise ValueError(f"{source}: unknown directive {key}")
        if key in result and key != "env":
            raise ValueError(f"{source}: repeated directive {key}")
        if key == "env":
            name, separator, text = value.partition("=")
            if not separator or not re.fullmatch(r"[A-Za-z_][A-Za-z_0-9]*", name):
                raise ValueError(f"{source}: env requires NAME=VALUE")
            result.setdefault(key, {})[name] = text
        else:
            result[key] = value

    return result


def invoke(command, *, env, timeout, stdin=None, cwd=None):
    return subprocess.run(list(map(str, command)), input=stdin, capture_output=True,
                          env=env, timeout=timeout, cwd=cwd)


def successful(command, **options):
    result = invoke(command, **options)
    if result.returncode:
        raise RuntimeError(f"{shlex.join(list(map(str, command)))}: exit {result.returncode}\n"
                           + result.stdout.decode(errors="replace") + result.stderr.decode(errors="replace"))
    return result


def check(result, case, phase):
    expected = (int(case.get("exit-code", "0")), json.loads(case.get("stdout", '""')).encode(),
                json.loads(case.get("stderr", '""')).encode())
    actual = result.returncode, result.stdout, result.stderr
    if "stderr-pattern" in case:
        if "stderr" in case:
            raise ValueError("stderr and stderr-pattern are mutually exclusive")
        pattern = json.loads(case["stderr-pattern"])
        if actual[:2] != expected[:2] or not re.fullmatch(pattern, result.stderr.decode(errors="replace")):
            raise RuntimeError(f"{phase}: got exit/stdout/stderr {actual!r}; expected {expected[:2]!r} and stderr pattern {pattern!r}")
        return
    if actual != expected:
        raise RuntimeError(f"{phase}: got exit/stdout/stderr {actual!r}; expected {expected!r}")


def run(source, prefix, tools):
    case = directives(source)
    timeout = float(case.get("timeout", "60"))
    env = dict(os.environ, **case.get("env", {}))
    # The native AOT command and the embedded reactor use the same mode.
    env["W2X_MEMORY_TAIL"] = "optimized"
    env["W2X_TAIL_PROFILE"] = ""
    aot_flags = shlex.split(case.get("aot-flags", ""))
    if aot_flags not in ([], ["--strict"], ["--asan"]):
        raise ValueError("aot-flags must be empty, --strict or --asan")
    if aot_flags == ["--strict"]:
        env["W2X_MEMORY_TAIL"] = "strict"
    run_flags = shlex.split(case.get("run-flags", ""))
    args = shlex.split(case.get("args", ""))
    cflags = ["-O2", "-std=c++23", "-fno-exceptions", "-fno-rtti", *shlex.split(case.get("compile-flags", ""))]
    maximum = int(case.get("max-memory", "4294967296"))
    if maximum <= 0 or maximum > 4294967296 or maximum % 65536:
        raise ValueError("max-memory must be a positive multiple of 65536 up to 4294967296")
    link = [flag if not flag.startswith("--max-memory=") else f"--max-memory={maximum}" for flag in LINK]
    prefix.parent.mkdir(parents=True, exist_ok=True)
    module = Path(str(prefix) + ".wasm")

    with tempfile.TemporaryDirectory(prefix="pizdos-case-") as temporary:
        work = Path(temporary)
        if case.get("host", "true") not in ("true", "false"):
            raise ValueError("host must be true or false")
        if case.get("freestanding", "false") not in ("true", "false"):
            raise ValueError("freestanding must be true or false")
        if case.get("host", "true") == "true":
            reference = work / "reference"
            successful([tools["cc"], *tools["host_cflags"], "-x", "c++", *cflags, source,
                        "-o", reference, *tools["host_ldflags"]], env=env, timeout=timeout)
            host_env = dict(env, **{name: value.replace("$ROOT", str(work)) for name, value in case.get("env", {}).items()})
            check(invoke([reference, *args], env=host_env, timeout=timeout, cwd=work), case, "native reference")

        with tarfile.open(tools["cxx_headers"]) as archive:
            archive.extractall(work / "cxx", filter="data")
        obj = work / "case.o"
        successful([tools["cc"], *wasm.CFLAGS, *cflags, "-nostdinc++",
                    "-isystem", work / "cxx/include/c++/v1", "-isystem", tools["c_headers"],
                    "-c", source, "-o", obj], env=env, timeout=timeout)
        libraries = [] if case.get("freestanding", "false") == "true" else [tools["libcxx"], tools["libc"]]
        successful([tools["ld"], *link, "-o", module, obj, *libraries], env=env, timeout=timeout)

        native = Path(str(prefix) + ".native.o")
        embedded = Path(str(prefix) + ".embedded.o")
        asan = ["--asan"] if aot_flags == ["--asan"] else []
        compiled = successful([tools["aot"], *aot_flags], stdin=module.read_bytes(), env=env, timeout=timeout)
        native.write_bytes(compiled.stdout)
        successful([tools["vm"], *asan, "-o", embedded, module], env=env, timeout=timeout)
        if native.read_bytes() != embedded.read_bytes():
            raise RuntimeError("native and Wasm AOT produced different objects")

        with tempfile.TemporaryDirectory(prefix="pizdos-root-") as directory:
            root = Path(directory)
            (root / "bin").mkdir()
            (root / "tmp").mkdir()
            executable = root / "bin/check"
            executable.write_bytes(exe.pack(module.read_bytes()))
            executable.chmod(0o755)
            run_args = [argument.replace("$ROOT", str(root)) for argument in args]
            run_env = dict(env, **{name: value.replace("$ROOT", str(root)) for name, value in case.get("env", {}).items()})
            flags = [flag.replace("$ROOT", str(root)) for flag in run_flags]
            result = invoke([tools["vm"], "run", "--root", root, "-a", "check", *flags,
                             "--", executable, *run_args], env=run_env, timeout=timeout)
            check(result, case, "kernel")

    Path(str(prefix) + ".stamp").write_text("native reference, native and Wasm AOT objects, kernel: passed\n")
    print(f"{source.stem}: native/Wasm AOT behaviour agrees", flush=True)


if __name__ == "__main__":
    try:
        run(Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve(), json.loads(sys.argv[3]))
    except (RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
        sys.exit(str(error))
