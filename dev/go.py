#!/usr/bin/env python3
"""Fetch a Go program's modules, or build the program.

    go.py download <go> <source dir> <modules.tar>
    go.py build <go> <source dir> <output> [--wasip1] [--modules <modules.tar>]

download runs `go mod download` on the directory's go.mod and go.sum, which
checks every module against go.sum, and keeps the module cache's download
directory as a deterministic tarball. build compiles a copy of the
directory's .go files, go.mod and go.sum, natively or with --wasip1 as a
WASI module; its modules come from that tarball as the module proxy, with no
network.
"""
import os
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


def copy(source, tree, code=True):
    tree.mkdir(parents=True)

    for path in Path(source).iterdir():
        if code and path.suffix == ".go" and not path.name.endswith("_test.go") or path.name in ("go.mod", "go.sum"):
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


def build(go, source, output, *flags, work):
    flags = list(flags)
    tree = work / "src"
    copy(source, tree)
    env = environment(work)

    if "--modules" in flags:
        at = flags.index("--modules")
        proxy = work / "proxy"

        with tarfile.open(flags[at + 1]) as archive:
            archive.extractall(proxy, filter="data")

        env["GOPROXY"] = f"file://{proxy}"
        del flags[at:at + 2]

    if "--wasip1" in flags:
        env.update(GOOS="wasip1", GOARCH="wasm")
        flags.remove("--wasip1")

    if flags:
        raise SystemExit(f"unknown arguments {flags}")

    output = Path(output).resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([go, "build", "-trimpath", "-o", str(output), "."], cwd=tree, env=env, check=True)


def main():
    mode, *args = sys.argv[1:]
    work = Path(os.environ.get("TMPDIR") or "/tmp") / "go"
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)

    if mode == "download":
        download(*args, work)
    elif mode == "build":
        build(*args, work=work)
    else:
        raise SystemExit(f"unknown mode {mode}")


if __name__ == "__main__":
    main()
