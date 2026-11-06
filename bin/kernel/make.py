#!/usr/bin/env python3
"""Fetch the kernel's Go modules, or build the kernel from them.

    make.py download <go> <go.mod> <go.sum> <modules.tar>
    make.py build <go> <modules.tar> <source dir> <rootfs.wasm> <kernel>

download runs `go mod download`, which checks every module against go.sum,
and keeps the module cache's download directory as a tarball. build serves
that tarball as the module proxy, with no network, to a copy of the sources
that has rootfs.wasm, the module the kernel embeds, next to them.
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
    })
    env.update(extra)

    return env


def download(go, go_mod, go_sum, output, work):
    module = work / "module"
    module.mkdir()
    shutil.copy(go_mod, module / "go.mod")
    shutil.copy(go_sum, module / "go.sum")
    env = environment(work, GOPROXY="https://proxy.golang.org", GOSUMDB="sum.golang.org")
    subprocess.run([go, "mod", "download"], cwd=module, env=env, check=True)
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


def build(go, modules, source, rootfs, output, work):
    proxy = work / "proxy"

    with tarfile.open(modules) as archive:
        archive.extractall(proxy, filter="data")

    tree = work / "src"
    shutil.copytree(source, tree, ignore=shutil.ignore_patterns("*.py", "*.md"))
    shutil.copy(rootfs, tree / "rootfs.wasm")
    env = environment(work, GOPROXY=f"file://{proxy}", GOSUMDB="off")
    Path(output).parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([go, "build", "-trimpath", "-o", str(Path(output).resolve()), "."], cwd=tree, env=env, check=True)


def main():
    mode, *args = sys.argv[1:]
    work = Path(os.environ.get("TMPDIR") or "/tmp") / "kernel"
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    {"download": download, "build": build}[mode](*args, work)


if __name__ == "__main__":
    main()
