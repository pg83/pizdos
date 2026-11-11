#!/usr/bin/env python3
"""Validate ELF objects through the public VM command, including bad inputs.

    test_objects.py <vm> <native-aot>
"""
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile


def uleb(value):
    out = bytearray()
    while value >= 128:
        out.append((value & 127) | 128)
        value >>= 7
    out.append(value)
    return bytes(out)


def wasm_section(kind, body):
    return bytes([kind]) + uleb(len(body)) + body


def changed(data, at, layout, *values):
    out = bytearray(data)
    struct.pack_into("<" + layout, out, at, *values)
    return bytes(out)


class Object:
    def __init__(self, data):
        self.data = data
        self.headers = struct.unpack_from("<Q", data, 40)[0]
        self.count, self.names = struct.unpack_from("<HH", data, 60)
        self.sections = [struct.unpack_from("<IIQQQQIIQQ", data, self.headers + 64 * i) for i in range(self.count)]
        names = self.contents(self.names)
        self.named = {names[s[0]:].split(b"\0", 1)[0].decode(): i for i, s in enumerate(self.sections)}

    def contents(self, section):
        if isinstance(section, str):
            section = self.named[section]
        header = self.sections[section]
        return self.data[header[4]:header[4] + header[5]]

    def header(self, name):
        return self.headers + 64 * self.named[name]

    def replaced(self, name, contents):
        out = bytearray(self.data)
        offset = len(out)
        out.extend(contents)
        struct.pack_into("<QQ", out, self.header(name) + 24, offset, len(contents))
        return bytes(out)

    def note(self, descriptor):
        owner = b"wasm2o\0"
        contents = struct.pack("<III", len(owner), len(descriptor), 1)
        contents += owner + b"\0" * (-len(owner) % 4)
        contents += descriptor + b"\0" * (-len(descriptor) % 4)
        return self.replaced(".note.wasm2o", contents)


def main():
    vm, native = sys.argv[1:]
    env = dict(os.environ, W2X_MEMORY_TAIL="optimized", W2X_TAIL_PROFILE="")
    module = b"\0asm\1\0\0\0" + wasm_section(1, b"\1\x60\0\0") + wasm_section(3, b"\1\0")
    module += wasm_section(7, b"\1\6_start\0\0") + wasm_section(10, b"\1\2\0\x0b")
    compiled = subprocess.run([native], input=module, capture_output=True, env=env, timeout=60)
    assert compiled.returncode == 0, compiled.stderr
    data = compiled.stdout
    obj = Object(data)
    cases = []

    def bad(name, contents, diagnostic):
        cases.append((name, contents, diagnostic))

    bad("short-header", data[:32], "not an x86-64 relocatable object")
    bad("class", changed(data, 4, "B", 1), "not an x86-64 relocatable object")
    bad("machine", changed(data, 18, "H", 3), "not an x86-64 relocatable object")
    bad("headers", changed(data, 40, "Q", len(data) + 1), "malformed section headers")
    bad("names-index", changed(data, 62, "H", obj.count), "malformed section headers")
    bad("section-offset", changed(data, obj.header(".text") + 24, "Q", len(data) + 1), "out of the file")
    bad("section-size", changed(data, obj.header(".text") + 32, "Q", 1 << 63), "out of the file")
    for name in (".text", ".symtab"):
        bad("missing-" + name[1:], changed(data, obj.header(name), "I", 0), "no text or symbol table")
    bad("symtab-link", changed(data, obj.header(".symtab") + 40, "I", obj.count), "no text or symbol table")
    bad("symtab-size", changed(data, obj.header(".symtab") + 32, "Q", 1), "no text or symbol table")
    symbols = obj.contents(".symtab")
    symbols_at = obj.sections[obj.named[".symtab"]][4]
    bad("symbol-name", changed(data, symbols_at, "I", 0xffffffff), "string out of its table")
    function = next(i for i in range(0, len(symbols), 24) if symbols[i + 4] & 15 == 2 and struct.unpack_from("<H", symbols, i + 6)[0] == obj.named[".text"])
    bad("function-offset", changed(data, symbols_at + function + 8, "Q", len(obj.contents(".text")) + 1), "out of the text")
    for name in (".note.wasm2o", ".eh_frame"):
        bad("missing-" + name[1:], changed(data, obj.header(name), "I", 0), "no " + name)
    relocs = obj.contents(".rela.text")
    relocs_at = obj.sections[obj.named[".rela.text"]][4]
    bad("relocation-link", changed(data, obj.header(".rela.text") + 40, "I", obj.names), "malformed relocations")
    bad("relocation-size", changed(data, obj.header(".rela.text") + 32, "Q", 1), "malformed relocations")
    bad("relocation-symbol", changed(data, relocs_at + 8, "Q", (len(symbols) // 24 + 1) << 32 | 9), "relocation to symbol")
    bad("relocation-type", changed(data, relocs_at + 8, "I", 65535), "unsupported relocation type")
    bad("external-relative", changed(data, relocs_at + 8, "I", 2), "pc-relative relocation to external")
    bad("note-padding", obj.replaced(".note.wasm2o", b"\0" * 12), "no wasm2o note")
    note = obj.contents(".note.wasm2o")
    bad("note-length", obj.replaced(".note.wasm2o", changed(note, 4, "I", len(note) + 1)), "no wasm2o note")
    bad("note-name-overflow", obj.replaced(".note.wasm2o", changed(note, 0, "I", 0xffffffff)), "no wasm2o note")
    bad("metadata-short", obj.note(b"\x80"), "malformed object metadata")
    bad("metadata-integer", obj.note(uleb(1 << 32)), "malformed object metadata")
    bad("metadata-count", obj.note(uleb(128) + b"\0" + uleb(1 << 20)), "malformed object metadata")
    prefix = uleb(128) + b"\0" * 5
    bad("metadata-memory", obj.note(prefix + b"\1"), "memory metadata without a memory")
    bad("metadata-trailing", obj.note(prefix + b"\0\0"), "trailing object metadata")
    memory = uleb(128) + b"\1\1\1\0" + b"\0" * 4
    bad("metadata-tail", obj.note(memory + b"\1" + uleb(512) + uleb(96)), "unsupported memory tail")
    bad("metadata-profile", obj.note(memory + b"\2" + uleb(64) + b"\1"), "invalid memory tail profile")
    bad("metadata-shadow", obj.note(memory + b"\4\1"), "invalid shadow slot")
    bad("metadata-lines", obj.note(prefix + b"\10\0\1\0\0\1"), "invalid line table")
    eh = obj.contents(".eh_frame")
    bad("eh-length", obj.replaced(".eh_frame", changed(eh, 0, "I", 0xffffffff)), "malformed .eh_frame")
    bad("eh-short-record", obj.replaced(".eh_frame", b"\1\0\0\0\0"), "malformed .eh_frame")
    bad("eh-short-leb", obj.replaced(".eh_frame", struct.pack("<II", 6, 0) + b"\1\0"), "malformed .eh_frame")
    cie = struct.pack("<I", 0) + b"\1zR\0\1\x78\x10\1"
    bad("eh-short-augmentation", obj.replaced(".eh_frame", struct.pack("<I", len(cie)) + cie), "malformed .eh_frame")

    with tempfile.TemporaryDirectory(prefix="pizdos-objects-") as directory:
        work = Path(directory)
        source, embedded = work / "input.wasm", work / "embedded.o"
        source.write_bytes(module)
        result = subprocess.run([vm, "-o", str(embedded), str(source)], capture_output=True, env=env, timeout=60)
        assert result.returncode == 0 and embedded.read_bytes() == data, result.stderr

        def run(contents):
            program = work / "input.o"
            program.write_bytes(contents)
            return subprocess.run([vm, "run", "--", str(program)], capture_output=True, env=env, timeout=30)

        result = run(data)
        assert (result.returncode, result.stdout, result.stderr) == (0, b"", b""), result
        failures = []
        for name, contents, diagnostic in cases:
            result = run(contents)
            stderr = result.stderr.decode(errors="replace")
            if result.returncode == 0 or diagnostic not in stderr or "panic:" in stderr:
                failures.append(f"{name}: exit {result.returncode}; expected {diagnostic!r}\n{stderr}")
        if failures:
            sys.exit("\n".join(failures))
    print(f"objects: native/Wasm AOT agreement, valid execution and {len(cases)} malformed objects rejected")


if __name__ == "__main__":
    main()
