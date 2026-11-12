#!/usr/bin/env python3
"""Fetch a Go program's modules, or build the program.

    go.py download <go> <source dir> <modules.tar>
    go.py build <go> <source dir> <output> [--wasip1] [--cover] [--tags <tags>] [--ldflags <flags>] [--modules <modules.tar>] [--runtime <dir>] [--embed <file>]
    go.py test <go> <source dir> <stamp> [--runtime <dir>] [--embed <file>]

download runs `go mod download` on the directory's go.mod and go.sum, which
checks every module against go.sum, and keeps the module cache's download
directory as a deterministic tarball. build compiles a copy of the
directory's .go and .s files, go.mod and go.sum, natively or with --wasip1 as a
WASI module; its modules come from that tarball as the module proxy, with no
network. test runs the directory's tests, its _test.go files included, and
touches the stamp when they pass. --runtime builds with a fork of package
runtime: the directory's .go files join it and its edits.py edits its
files, as an overlay. --embed stages a dependency output next to the Go
sources for go:embed; it never modifies the source directory.
"""
import json
import os
import runpy
import shutil
import subprocess
import sys
import tarfile
from pathlib import Path


def environment(work, **extra):
    env = dict(os.environ)
    env.update({
        "GOCACHE": str(work / "gocache"),
        "GOMODCACHE": str(work / "mod"),
        "GOPATH": str(work / "gopath"),
        "HOME": str(work),
        "GOTOOLCHAIN": "local",
        "GOFLAGS": "-modcacherw -mod=readonly -buildvcs=false",
        "CGO_ENABLED": "0",
        "GOPROXY": "off",
        "GOSUMDB": "off",
    })
    env.update(extra)

    return env


def copy(source, tree, code=True, tests=False):
    tree.mkdir(parents=True)

    for path in Path(source).iterdir():
        if code and path.suffix in (".go", ".s") and (tests or not path.name.endswith("_test.go")) or path.name in ("go.mod", "go.sum"):
            shutil.copy(path, tree / path.name)


def download(go, source, output, work):
    copy(source, work / "src", code=False)
    env = environment(work, GOPROXY="https://proxy.golang.org", GOSUMDB="sum.golang.org")

    # Go trusts only SSL_CERT_FILE when it is set; one naming no file (ix run
    # sets it to `nowhere`) is dropped, so the system's trust store is used.
    if not os.path.isfile(env.get("SSL_CERT_FILE", "")):
        env.pop("SSL_CERT_FILE", None)

    subprocess.run([go, "mod", "download"], cwd=work / "src", env=env, check=True)
    cache = work / "mod" / "cache" / "download"

    def reset(info):
        info.mtime = 0
        info.uid = info.gid = 0
        info.uname = info.gname = ""

        return info

    Path(output).parent.mkdir(parents=True, exist_ok=True)

    with tarfile.open(output, "w") as archive:
        for path in sorted(cache.rglob("*")):
            if path.is_file() and not path.name.endswith(".lock"):
                archive.add(path, arcname=path.relative_to(cache).as_posix(), recursive=False, filter=reset)


def runtime_overlay(go, fork, work, env):
    """The overlay of a runtime fork: its files and its edits of $GOROOT's."""
    goroot = Path(subprocess.run([go, "env", "GOROOT"], env=env, capture_output=True, text=True, check=True).stdout.strip())
    runtime = goroot / "src" / "runtime"
    edited = work / "runtime"
    edited.mkdir()
    texts = {}

    for name, anchor, text in runpy.run_path(str(Path(fork) / "edits.py"))["EDITS"]:
        code = texts.get(name) or (runtime / name).read_text()

        if code.count(anchor) != 1:
            raise SystemExit(f"{runtime / name}: the anchor of an edit is not there exactly once")

        texts[name] = code.replace(anchor, text)

    replace = {}

    for name, code in texts.items():
        (edited / name).write_text(code)
        replace[str(runtime / name)] = str(edited / name)

    for path in sorted(Path(fork).glob("*.go")):
        replace[str(runtime / path.name)] = str(path.resolve())

    overlay = work / "overlay.json"
    overlay.write_text(json.dumps({"Replace": replace}))

    return f"-overlay={overlay}"


def take(flags, name):
    """Removes an option and its value from flags and returns the value."""
    if name not in flags:
        return None

    at = flags.index(name)
    value = flags[at + 1]
    del flags[at:at + 2]

    return value


def embed(flags, tree):
    while "--embed" in flags:
        path = Path(take(flags, "--embed"))
        shutil.copyfile(path, tree / path.name)


def build(go, source, output, *flags, work):
    flags = list(flags)
    tree = work / "src"
    copy(source, tree)
    embed(flags, tree)
    env = environment(work)
    fork = take(flags, "--runtime")
    tags = take(flags, "--tags")
    options = ["-tags=" + tags] if tags else []
    ldflags = take(flags, "--ldflags")

    if ldflags:
        options.append("-ldflags=" + ldflags)

    if "--cover" in flags:
        flags.remove("--cover")
        options.extend(["-cover", "-covermode=atomic"])

    modules = take(flags, "--modules")

    if modules:
        proxy = work / "proxy"

        with tarfile.open(modules) as archive:
            archive.extractall(proxy, filter="data")

        env["GOPROXY"] = f"file://{proxy}"

    if "--wasip1" in flags:
        env.update(GOOS="wasip1", GOARCH="wasm")
        flags.remove("--wasip1")

    if flags:
        raise SystemExit(f"unknown arguments {flags}")

    overlay = [runtime_overlay(go, fork, work, env)] if fork else []
    output = Path(output).resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([go, "build", "-trimpath", *options, *overlay, "-o", str(output), "."], cwd=tree, env=env, check=True)


def test(go, source, stamp, *flags, work):
    flags = list(flags)
    fork = take(flags, "--runtime")
    copy(source, work / "src", tests=True)
    embed(flags, work / "src")

    if flags:
        raise SystemExit(f"unknown arguments {flags}")

    env = environment(work)
    overlay = [runtime_overlay(go, fork, work, env)] if fork else []
    subprocess.run([go, "test", "-count=1", "-timeout=300s", *overlay, "."], cwd=work / "src", env=env, check=True)
    Path(stamp).parent.mkdir(parents=True, exist_ok=True)
    Path(stamp).touch()


def main():
    mode, *args = sys.argv[1:]
    work = Path(os.environ.get("TMPDIR") or "/tmp") / "go"
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)

    if mode == "download":
        download(*args, work)
    elif mode == "build":
        build(*args, work=work)
    elif mode == "test":
        test(*args, work=work)
    else:
        raise SystemExit(f"unknown mode {mode}")


if __name__ == "__main__":
    main()
