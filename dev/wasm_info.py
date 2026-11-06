#!/usr/bin/env python3
"""The import and export sections of a wasm module, one entry per line.

    wasm_info.py <module.wasm> <output.txt>

Lines are `import <module> <name> <kind>` and `export <name> <kind>`; the
imports of a program are the kernel ABI it needs. layout() reads the rest of
a module the way a loader needs it.
"""
import dataclasses
import struct
import sys
from pathlib import Path

KINDS = {0: "func", 1: "table", 2: "memory", 3: "global", 4: "tag"}
VALUES = {0x7F: "i32", 0x7E: "i64", 0x7D: "f32", 0x7C: "f64"}


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

    def sleb(self):
        result = 0
        shift = 0

        while True:
            byte = self.byte()
            result |= (byte & 0x7F) << shift
            shift += 7

            if not byte & 0x80:
                if byte & 0x40:
                    result -= 1 << shift

                return result

    def bytes(self, length):
        value = self.data[self.at:self.at + length]
        self.at += length

        return value

    def name(self):
        return self.bytes(self.uleb()).decode()

    def limits(self):
        flags = self.byte()
        minimum = self.uleb()

        return minimum, self.uleb() if flags & 1 else None

    def constant(self):
        """A constant expression's raw bits."""
        opcode = self.byte()

        if opcode == 0x41:
            value = self.sleb() & 0xFFFFFFFF
        elif opcode == 0x42:
            value = self.sleb() & 0xFFFFFFFFFFFFFFFF
        elif opcode == 0x43:
            value = struct.unpack("<I", self.bytes(4))[0]
        elif opcode == 0x44:
            value = struct.unpack("<Q", self.bytes(8))[0]
        else:
            raise SystemExit(f"constant expression opcode {opcode:#x}")

        if self.byte() != 0x0B:
            raise SystemExit("constant expression longer than one instruction")

        return value


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


@dataclasses.dataclass
class Layout:
    types: list = dataclasses.field(default_factory=list)  # (params, results)
    functions: list = dataclasses.field(default_factory=list)  # type index, imports first
    imports: list = dataclasses.field(default_factory=list)  # (module, name) of each imported function
    exports: dict = dataclasses.field(default_factory=dict)  # function name to index
    globals: list = dataclasses.field(default_factory=list)  # (type, mutable, bits)
    memory: tuple = (0, None)  # pages
    elements: list = dataclasses.field(default_factory=list)  # (offset, function indices)
    data: list = dataclasses.field(default_factory=list)  # (offset, bytes) of active segments


def layout(data):
    """Types, functions, globals, the memory, and the active segments of a module."""
    result = Layout()

    for section, reader in sections(data):
        if section == 1:
            for _ in range(reader.uleb()):
                if reader.byte() != 0x60:
                    raise SystemExit("type is not a function type")

                params = [VALUES[reader.byte()] for _ in range(reader.uleb())]
                results = [VALUES[reader.byte()] for _ in range(reader.uleb())]
                result.types.append((params, results))
        elif section == 2:
            for _ in range(reader.uleb()):
                module = reader.name()
                name = reader.name()
                kind = reader.byte()

                if kind != 0:
                    raise SystemExit(f"import {module}.{name} is a {KINDS[kind]}")

                result.functions.append(reader.uleb())
                result.imports.append((module, name))
        elif section == 3:
            result.functions += [reader.uleb() for _ in range(reader.uleb())]
        elif section == 5:
            for _ in range(reader.uleb()):
                result.memory = reader.limits()
        elif section == 6:
            for _ in range(reader.uleb()):
                ty = VALUES[reader.byte()]
                mutable = bool(reader.byte())
                result.globals.append((ty, mutable, reader.constant()))
        elif section == 7:
            for _ in range(reader.uleb()):
                name = reader.name()
                kind = reader.byte()
                index = reader.uleb()

                if kind == 0:
                    result.exports[name] = index
        elif section == 9:
            for _ in range(reader.uleb()):
                if reader.uleb() != 0:
                    raise SystemExit("only active element segments of table 0 are read")

                offset = reader.constant()
                result.elements.append((offset, [reader.uleb() for _ in range(reader.uleb())]))
        elif section == 11:
            for _ in range(reader.uleb()):
                flags = reader.uleb()

                if flags == 1:
                    reader.bytes(reader.uleb())
                    continue

                if flags != 0:
                    raise SystemExit(f"data segment flags {flags}")

                offset = reader.constant()
                result.data.append((offset, reader.bytes(reader.uleb())))

    return result


def main():
    imports, exports = module_info(Path(sys.argv[1]).read_bytes())
    lines = [f"import {module} {name} {kind}" for module, name, kind in sorted(imports)]
    lines += [f"export {name} {kind}" for name, kind in sorted(exports)]
    Path(sys.argv[2]).write_text("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
