#!/usr/bin/env python3
"""Check spec-command results, diagnostics and malformed actions via JSON.

    test_spec_cli.py <vm> <native-aot>
"""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def leb(value):
    out = bytearray()
    while value >= 128:
        out.append(value & 127 | 128)
        value >>= 7
    return bytes(out + bytes([value]))


def text(value):
    encoded = value.encode()
    return leb(len(encoded)) + encoded


def section(kind, body):
    return bytes([kind]) + leb(len(body)) + body


def exported(name, kind, index):
    return text(name) + bytes([kind]) + leb(index)


def base_module(start=False):
    types = b"\2\x60\1\x7f\1\x7f\x60\0\0"
    module = b"\0asm\1\0\0\0" + section(1, types) + section(3, b"\2\0\1")
    module += section(4, b"\1\x70\1\0\4") + section(5, b"\1\1\1\2")
    globals = [("gi32", b"\x7f\0\x41\7\x0b"), ("gi64", b"\x7e\0\x42\11\x0b"),
               ("gf32", b"\x7d\0\x43\0\0\x80\x3f\x0b"),
               ("gf64", b"\x7c\0\x44\0\0\0\0\0\0\0\x40\x0b"),
               ("gr", b"\x70\0\xd0\x70\x0b"), ("ge", b"\x6f\0\xd0\x6f\x0b"),
               ("nonnull", b"\x70\0\xd2\0\x0b")]
    module += section(6, leb(len(globals)) + b"".join(value for _, value in globals))
    exports = [exported("id", 0, 0), exported("trap", 0, 1), exported("memory", 2, 0), exported("table", 1, 0)]
    exports += [exported(name, 3, i) for i, (name, _) in enumerate(globals)]
    module += section(7, leb(len(exports)) + b"".join(exports))
    if start:
        module += section(8, b"\1")
    return module + section(10, b"\2\4\0\x20\0\x0b\3\0\0\x0b")


def importing(kind, owner, name, signature=False):
    module = b"\0asm\1\0\0\0"
    if kind == 0:
        module += section(1, b"\1\x60\0\0" if signature else b"\1\x60\1\x7f\1\x7f")
    entity = {0: b"\0", 1: b"\x70\1\0\4", 2: b"\1\1\2", 3: b"\x7f\0"}[kind]
    module += section(2, b"\1" + text(owner) + text(name) + bytes([kind]) + entity)
    return module + section(7, b"\1" + exported("alias", kind, 0))


def action(field="id", arguments=None, kind="invoke"):
    return {"type": kind, "module": "main", "field": field,
            "args": [{"type": "i32", "value": "7"}] if arguments is None else arguments}


def main():
    vm, native = sys.argv[1:]
    env = dict(os.environ, W2X_MEMORY_TAIL="strict", W2X_TAIL_PROFILE="")
    with tempfile.TemporaryDirectory(prefix="pizdos-spec-cli-") as directory:
        work = Path(directory)
        modules = {"base": base_module(), "start-trap": base_module(True),
                   "global-alias": importing(3, "shared", "gi32")}
        for kind in range(4):
            modules[f"missing-module-{kind}"] = importing(kind, "unknown", "missing")
            modules[f"missing-field-{kind}"] = importing(kind, "shared", "missing")
            modules[f"missing-spectest-{kind}"] = importing(kind, "spectest", "missing")
        modules["wrong-signature"] = importing(0, "shared", "id", True)
        for name, data in modules.items():
            source, embedded = work / f"{name}.wasm", work / "embedded.o"
            source.write_bytes(data)
            compiled = subprocess.run([native, "--strict"], input=data, capture_output=True, env=env, timeout=60)
            assert compiled.returncode == 0, (name, compiled.stderr)
            result = subprocess.run([vm, "-o", str(embedded), str(source)], capture_output=True, env=env, timeout=60)
            assert result.returncode == 0 and embedded.read_bytes() == compiled.stdout, (name, result)

        base = [{"type": "module", "name": "main", "filename": "base.wasm"},
                {"type": "register", "name": "main", "as": "shared"}]

        def run(commands, counts, diagnostics=()):
            document = work / "commands.json"
            document.write_text(json.dumps({"commands": [*base, *commands]}))
            result = subprocess.run([vm, "spec", str(document)], capture_output=True, env=env, timeout=30)
            assert result.returncode == 0 and result.stderr == b"", result
            output = result.stdout.decode()
            assert f"RESULT {counts[0] + 1} {counts[1]} {counts[2]}\n" in output, output
            assert "panic:" not in output and all(message in output for message in diagnostics), output

        good = []
        for field, kind, value in (("gi32", "i32", "7"), ("gi64", "i64", "9"), ("gf32", "f32", "1065353216"),
                                   ("gf64", "f64", "4611686018427387904"), ("gr", "funcref", "null"), ("ge", "externref", "null")):
            good.append({"type": "assert_return", "action": action(field, [], "get"), "expected": [{"type": kind, "value": value}]})
        good += [{"type": "assert_return", "action": action("nonnull", [], "get"), "expected": [{"type": "funcref"}]},
                 {"type": "assert_return", "action": action(), "expected": [{"type": "i32", "value": "7"}]},
                 {"type": "assert_trap", "action": action("trap", []), "text": "unreachable"}]
        run(good, (len(good), 0, 0))
        for argument in ({"type": "i32", "value": 7}, {"type": "i32", "value": "bad"},
                         {"type": "unknown", "value": "7"}, {"type": "f32", "value": "0"}):
            run([{"type": "action", "action": action(arguments=[argument])}], (0, 0, 1))
        for command in ({"type": "assert_return"}, {"type": "assert_trap"},
                        {"type": "action", "action": action(arguments=[])},
                        {"type": "action", "action": action("missing")},
                        {"type": "action", "action": action("missing", [], "get")},
                        {"type": "module", "module_type": "text"},
                        {"type": "assert_return", "action": action(), "expected": [{"type": "v128", "value": []}]}):
            run([command], (0, 0, 1))
        for expected in ([], [{"type": "i32", "value": 7}], [{"type": "i32", "value": "bad"}]):
            run([{"type": "assert_return", "action": action(), "expected": expected}], (0, 1, 0), ["FAIL"])
        run([{"type": "assert_trap", "action": action(), "text": "unreachable"}], (0, 1, 0), ["no trap"])
        run([{"type": "assert_trap", "action": action("trap", []), "text": "integer divide by zero"}],
            (0, 1, 0), ["trap 1, expected 4"])
        run([{"type": "assert_uninstantiable", "filename": "base.wasm"}], (0, 1, 0), ["instantiation did not trap"])
        run([{"type": "module", "filename": "start-trap.wasm"}], (0, 1, 0), ["instantiation trapped"])
        run([{"type": "assert_uninstantiable", "filename": "start-trap.wasm"}], (1, 0, 0))
        run([{"type": "module", "name": "alias", "filename": "global-alias.wasm"},
             {"type": "assert_return", "action": {"type": "get", "module": "alias", "field": "alias"},
              "expected": [{"type": "i32", "value": "7"}]}], (2, 0, 0))
        for name in modules:
            if name.startswith("missing-") or name == "wrong-signature":
                run([{"type": "module", "filename": name + ".wasm"}], (0, 0, 1), ["UNSUPPORTED"])
    print("spec CLI: native/Wasm objects agree, globals, imports, results, traps and malformed actions checked")


if __name__ == "__main__":
    main()
