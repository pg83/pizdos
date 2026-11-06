#!/usr/bin/env python3
"""Build rootfs.wasm: the Go module with its read-only layer; main.go has the format.

    pack.py <go> <source dir> <rootfs.wasm> <self path> [<path>=<file>]... [--links <file> <target>]...

<self path> names the module's own file. The layer is embedded as layer.bin,
which lands in the module as one contiguous run of bytes, and the module's
file is stored as the bytes around that run. Their size depends on the
layer's, so the module is built with a small placeholder to measure them,
then with a placeholder as large as the layer, again until the layer fits;
the placeholder's bytes are then overwritten by the layer. A --links file lists one absolute path per
line, each a symlink to <target>.
"""
import os
import shutil
import struct
import subprocess
import sys
from pathlib import Path

MAGIC = b"PZFS"
VERSION = 1
HEADER = 40
INODE = 12
DIRENT = 12
PATTERN = b"PZFSPAD!"
# Room for the sizes around the layer to grow with it: LEBs of segment and section sizes.
SLACK = 4096
S_IFDIR = 0o040755
S_IFREG = 0o100755
S_IFLNK = 0o120777


def align(value):
    return (value + 7) & ~7


class Tree:
    """Absolute paths to (mode, bytes); None for a directory."""

    def __init__(self):
        self.entries = {"/": None}

    def add(self, path, entry):
        if not path.startswith("/") or path in self.entries:
            raise SystemExit(f"bad or duplicate path {path}")

        parts = path.split("/")[1:]

        for depth in range(1, len(parts)):
            parent = "/" + "/".join(parts[:depth])

            if self.entries.setdefault(parent, None) is not None:
                raise SystemExit(f"{parent} is a file and a directory")

        self.entries[path] = entry


def name(path):
    return path.rsplit("/", 1)[1].encode()


def parent(path):
    return path.rsplit("/", 1)[0] or "/"


def layer(tree, self_path, prefix, suffix, size=None):
    """The layer's bytes; its length alone when prefix and suffix are sizes."""
    paths = ["/", *sorted(path for path in tree.entries if path != "/")]
    inodes = {path: index for index, path in enumerate(paths)}
    children = {path: [] for path in paths if tree.entries[path] is None}

    for path in paths[1:]:
        children[parent(path)].append(path)

    for entries in children.values():
        entries.sort(key=name)

    at = HEADER + INODE * len(paths)
    dirents = {}

    for path in paths:
        if path in children:
            dirents[path] = at
            at += DIRENT * len(children[path])

    names = {}

    for path in paths[1:]:
        names[path] = at
        at += len(name(path))

    prefix_size = prefix if isinstance(prefix, int) else len(prefix)
    suffix_size = suffix if isinstance(suffix, int) else len(suffix)
    prefix_at = align(at)
    suffix_at = align(prefix_at + prefix_size)
    at = suffix_at + suffix_size
    contents = {}

    for path in paths:
        entry = tree.entries[path]

        if entry is not None and path != self_path:
            at = align(at)
            contents[path] = at
            at += len(entry[1])

    if size is None:
        return at

    assert at <= size, (at, size)
    out = bytearray(size)
    header = (VERSION, size, len(paths), inodes[self_path], prefix_at, prefix_size, suffix_at, suffix_size, 0)
    out[:HEADER] = MAGIC + struct.pack("<9I", *header)
    out[prefix_at:prefix_at + prefix_size] = prefix
    out[suffix_at:suffix_at + suffix_size] = suffix

    for path in paths:
        at = HEADER + INODE * inodes[path]

        if path in children:
            struct.pack_into("<3I", out, at, S_IFDIR, dirents[path], len(children[path]))

            for index, child in enumerate(children[path]):
                struct.pack_into("<3I", out, dirents[path] + DIRENT * index, names[child], len(name(child)), inodes[child])
        elif path == self_path:
            struct.pack_into("<3I", out, at, S_IFREG, 0, 0)
        else:
            mode, data = tree.entries[path]
            struct.pack_into("<3I", out, at, mode, contents[path], len(data))
            out[contents[path]:contents[path] + len(data)] = data

        if path != "/":
            out[names[path]:names[path] + len(name(path))] = name(path)

    return out


def build(go, source, work, placeholder):
    """The module built with layer.bin holding placeholder."""
    tree = work / "src"
    shutil.rmtree(tree, ignore_errors=True)
    shutil.copytree(source, tree, ignore=shutil.ignore_patterns("*.py", "layer.bin"))
    (tree / "layer.bin").write_bytes(placeholder)
    env = dict(os.environ)
    env.update({
        "GOOS": "wasip1",
        "GOARCH": "wasm",
        "CGO_ENABLED": "0",
        "GOCACHE": str(work / "gocache"),
        "GOPATH": str(work / "gopath"),
        "HOME": str(work),
        "GOTOOLCHAIN": "local",
        "GOPROXY": "off",
        "GOFLAGS": "-buildvcs=false",
    })
    output = work / "rootfs.wasm"
    command = [go, "build", "-buildmode=c-shared", "-trimpath", "-ldflags=-s -w", "-o", str(output), "."]
    subprocess.run(command, cwd=tree, env=env, check=True)

    return output.read_bytes()


def main():
    go, source, output, self_path, *rest = sys.argv[1:]
    tree = Tree()
    tree.add(self_path, (S_IFREG, b""))

    while rest:
        if rest[0] == "--links":
            for path in Path(rest[1]).read_text().split():
                if path != rest[2]:
                    tree.add(path, (S_IFLNK, rest[2].encode()))

            rest = rest[3:]
        else:
            path, file = rest[0].split("=", 1)
            tree.add(path, (S_IFREG, Path(file).read_bytes()))
            rest = rest[1:]

    work = Path(os.environ.get("TMPDIR") or "/tmp") / "rootfs"
    work.mkdir(parents=True, exist_ok=True)
    around = len(build(go, source, work, PATTERN)) - len(PATTERN)

    # The bytes around the layer grow a little with it; build again until it fits.
    while True:
        size = align(layer(tree, self_path, around + SLACK, 0))
        placeholder = PATTERN * (size // len(PATTERN))
        module = build(go, source, work, placeholder)
        at = module.find(placeholder)

        if at < 0 or module.find(placeholder, at + 1) >= 0:
            raise SystemExit("layer.bin is not one contiguous run in the module")

        prefix, suffix = module[:at], module[at + size:]

        if layer(tree, self_path, len(prefix), len(suffix)) <= size:
            break

        around = len(prefix) + len(suffix)

    image = layer(tree, self_path, prefix, suffix, size)
    target = Path(output)
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(prefix + bytes(image) + suffix)


if __name__ == "__main__":
    main()
