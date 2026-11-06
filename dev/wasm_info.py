#!/usr/bin/env python3
"""The import and export sections of a wasm module, one entry per line.

    wasm_info.py <module.wasm> <output.txt>

Lines are `import <module> <name> <kind>` and `export <name> <kind>`; the
imports of a program are the kernel ABI it needs.
"""
import sys
from pathlib import Path

KINDS = {0: "func", 1: "table", 2: "memory", 3: "global", 4: "tag"}


class Reader:
    def __init__(self, data):
        self.data = data
        self.at = 0

    def byte(self):
        value = self.data[self.at]
        self.at += 1

        return value

    def uleb(self):
        result = 0
        shift = 0

        while True:
            byte = self.byte()
            result |= (byte & 0x7F) << shift
            shift += 7

            if not byte & 0x80:
                return result

    def name(self):
        length = self.uleb()
        value = self.data[self.at:self.at + length].decode()
        self.at += length

        return value

    def limits(self):
        flags = self.byte()
        self.uleb()

        if flags & 1:
            self.uleb()


def skip_import_description(reader, kind):
    if kind == 0:
        reader.uleb()
    elif kind == 1:
        reader.byte()
        reader.limits()
    elif kind == 2:
        reader.limits()
    elif kind == 3:
        reader.byte()
        reader.byte()
    elif kind == 4:
        reader.byte()
        reader.uleb()
    else:
        raise SystemExit(f"unknown import kind {kind}")


def sections(data):
    if data[:4] != b"\0asm":
        raise SystemExit("not a wasm module")

    reader = Reader(data)
    reader.at = 8

    while reader.at < len(data):
        section = reader.byte()
        size = reader.uleb()
        start = reader.at
        reader.at += size

        yield section, Reader(data[start:start + size])


def module_info(data):
    imports = []
    exports = []

    for section, reader in sections(data):
        if section == 2:
            for _ in range(reader.uleb()):
                module = reader.name()
                name = reader.name()
                kind = reader.byte()
                skip_import_description(reader, kind)
                imports.append((module, name, KINDS[kind]))
        elif section == 7:
            for _ in range(reader.uleb()):
                name = reader.name()
                kind = reader.byte()
                reader.uleb()
                exports.append((name, KINDS[kind]))

    return imports, exports


def main():
    imports, exports = module_info(Path(sys.argv[1]).read_bytes())
    lines = [f"import {module} {name} {kind}" for module, name, kind in sorted(imports)]
    lines += [f"export {name} {kind}" for name, kind in sorted(exports)]
    Path(sys.argv[2]).write_text("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
