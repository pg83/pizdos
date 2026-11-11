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
        return self.headers + 64 * (self.named[name] if isinstance(name, str) else name)

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

    def renamed(self, old, new):
        table = self.sections[self.named[".symtab"]][6]
        strings = self.contents(table)
        symbols = self.contents(".symtab")
        for at in range(0, len(symbols), 24):
            start = struct.unpack_from("<I", symbols, at)[0]
            if strings[start:].split(b"\0", 1)[0].decode() == old:
                out = self.replaced(table, strings + new.encode() + b"\0")
                return changed(out, self.sections[self.named[".symtab"]][4] + at, "I", len(strings))
        raise AssertionError(old)


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
    for value in (0xffffffff, len(obj.contents(".text")) - 1, 1 << 32):
        bad(f"relocation-offset-{value}", changed(data, relocs_at, "Q", value), "relocation out of section")
    external = struct.unpack_from("<Q", relocs, 8)[0] >> 32
    bad("relocation-section", changed(data, symbols_at + 24 * external + 6, "H", obj.named[".eh_frame"]), "relocation into section")
    bad("relocation-section-index", changed(data, symbols_at + 24 * external + 6, "H", 65535), "relocation into section")
    bad("GOT-local", changed(data, symbols_at + 24 * external + 6, "H", obj.named[".text"]), "GOT relocation to a symbol of the object")
    relative = changed(data, relocs_at + 8, "Q", (function // 24) << 32 | 2)
    bad("relative-overflow", changed(relative, relocs_at + 16, "q", 1 << 50), "relocation out of range")
    bad("missing-init", obj.renamed("w2o.init", "w2o.gone"), "no w2o.init")
    bad("unresolved-external", obj.renamed("w2o.morestack", "unknown.external"), "unresolved symbol unknown.external")
    for name in ("w2o.export._..", "w2o.export._GG..", "w2o.export._5Fstart...", "w2o.export._5Fstart.z."):
        bad("export-" + name, obj.renamed("w2o.export._5Fstart..", name), "no exported function _start")
    for name in ("w2o.export._5Fstart.i.", "w2o.export._5Fstart..i"):
        bad("export-signature-" + name, obj.renamed("w2o.export._5Fstart..", name), "_start must have no parameters or results")
    bad("export-vector", obj.renamed("w2o.export._5Fstart..", "w2o.export._5Fstart.v."), "unsupported value type v128")
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
    for least, most in ((2, 1), (1, 65537), (1 << 40, 1 << 40)):
        descriptor = uleb(128) + b"\1" + uleb(least) + uleb(most) + b"\0" * 6
        bad(f"metadata-memory-limits-{least}-{most}", obj.note(descriptor), f"memory of {least}..{most} pages")
    table = uleb(128) + b"\0\1" + uleb(96) + uleb((1 << 24) + 1) + uleb((1 << 24) + 1) + b"\0" * 5
    bad("metadata-table-limits", obj.note(table), "table of 16777217 elements")
    bad("metadata-tail", obj.note(memory + b"\1" + uleb(512) + uleb(96)), "unsupported memory tail")
    bad("metadata-profile", obj.note(memory + b"\2" + uleb(64) + b"\1"), "invalid memory tail profile")
    bad("metadata-shadow", obj.note(memory + b"\4\1"), "invalid shadow slot")
    bad("metadata-lines", obj.note(prefix + b"\10\0\1\0\0\1"), "invalid line table")
    eh = obj.contents(".eh_frame")
    bad("eh-length", obj.replaced(".eh_frame", changed(eh, 0, "I", 0xffffffff)), "malformed .eh_frame")

    def short_eh(contents):
        # These CIE-only fixtures have no FDE to relocate.
        return Object(obj.replaced(".eh_frame", contents)).replaced(".rela.eh_frame", b"")

    bad("eh-short-record", short_eh(b"\1\0\0\0\0"), "malformed .eh_frame")
    bad("eh-short-leb", short_eh(struct.pack("<II", 6, 0) + b"\1\0"), "malformed .eh_frame")
    cie = struct.pack("<I", 0) + b"\1zR\0\1\x78\x10\1"
    bad("eh-short-augmentation", short_eh(struct.pack("<I", len(cie)) + cie), "malformed .eh_frame")
    for at, value in ((10, ord("X")), (15, 2), (16, 0)):
        bad(f"eh-augmentation-{at}", obj.replaced(".eh_frame", changed(eh, at, "B", value)), "unsupported .eh_frame augmentation")
    first_fde = struct.unpack_from("<I", eh)[0] + 4
    bad("eh-unknown-cie", obj.replaced(".eh_frame", changed(eh, first_fde + 4, "I", 0x7fffffff)), "unsupported FDE")
    bad("eh-missing-relocations", obj.replaced(".rela.eh_frame", b""), "unsupported FDE")
    bad("eh-wrong-register", obj.replaced(".eh_frame", changed(eh, 18, "B", 6)), "a CFA not on rsp")
    bad("eh-unknown-opcode", obj.replaced(".eh_frame", changed(eh, 17, "B", 0x0f)), "unsupported CFA instruction")
    bad("eh-past-function", obj.replaced(".eh_frame", changed(eh, first_fde + 17, "B", 0x41)), "CFA instructions past their range")
    bad("eh-fde-augmentation-overflow", obj.replaced(".eh_frame", changed(eh, first_fde + 16, "B", 127)), "malformed .eh_frame")
    for code in (b"\x0e", b"\x0e\x80", b"\x02", b"\x03\1", b"\x04\1\2\3", b"\x0c\7"):
        record = struct.pack("<I", 0) + b"\1\0\1\x78\x10" + code
        bad("eh-truncated-cfa-" + code.hex(), short_eh(struct.pack("<I", len(record)) + record), "malformed .eh_frame")
    valid = [("original", data)]
    valid.append(("eh-terminator", obj.replaced(".eh_frame", eh + bytes(4))))
    # Insert transient changes at one PC, restoring the original rule.
    # The final stack map must be identical after adjacent rules merge.
    off = 0
    while off + 4 <= len(eh):
        size = struct.unpack_from("<I", eh, off)[0]
        record = eh[off:off + 4 + size]
        marker = b"\x47\x0e\x18"
        if marker in record and record.endswith(bytes(4)):
            rewritten = record.replace(marker, marker + b"\x0e\x10\x0e\x18", 1)[:-4]
            valid.append(("eh-repeated-cfa-pc", obj.replaced(".eh_frame", eh[:off] + rewritten + eh[off + 4 + size:])))
            break
        off += 4 + size
    assert len(valid) == 3, "fixture has no padded stack map"
    duplicate = changed(relocs[:24], 8, "I", 65535) + relocs
    valid.append(("later-relocation-wins", obj.replaced(".rela.text", duplicate)))
    valid.append(("unsorted-relocations", obj.replaced(".rela.text", b"".join(relocs[i:i + 24] for i in reversed(range(0, len(relocs), 24))))))
    extended = Object(obj.replaced(".text", obj.contents(".text") + b"\0" * 8))
    absolute = struct.pack("<QQq", len(obj.contents(".text")), external << 32 | 1, 0)
    valid.append(("absolute-external", extended.replaced(".rela.text", relocs + absolute)))

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

        for name, contents in valid:
            result = run(contents)
            assert (result.returncode, result.stdout, result.stderr) == (0, b"", b""), (name, result)
        failures = []
        for name, contents, diagnostic in cases:
            result = run(contents)
            stderr = result.stderr.decode(errors="replace")
            if result.returncode == 0 or diagnostic not in stderr or "panic:" in stderr:
                failures.append(f"{name}: exit {result.returncode}; expected {diagnostic!r}\n{stderr[:1800]}")
        if failures:
            sys.exit("\n".join(failures))
    print(f"objects: native/Wasm AOT agreement, {len(valid)} valid objects and {len(cases)} malformed objects rejected")


if __name__ == "__main__":
    main()
