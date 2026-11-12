#!/usr/bin/env python3
"""Round-trip numeric spec function references through tables and exports."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def commands(filename):
    def action(field, value=None):
        args = [] if value is None else [{"type": "funcref", "value": value}]
        return {"type": "invoke", "module": "main", "field": field, "args": args}

    def returned(field, value, typ, expected):
        return {"type": "assert_return", "action": action(field, value),
                "expected": [{"type": typ, "value": expected}]}

    checks = [
        {"type": "module", "name": "main", "filename": filename},
        returned("apply", "0", "i32", "42"),
        returned("apply", "1", "i32", "57"),
        returned("reference0", None, "funcref", "0"),
        returned("reference1", None, "funcref", "1"),
        returned("identity", "0", "funcref", "0"),
        returned("identity", "1", "funcref", "1"),
        returned("identity", "null", "funcref", "null"),
        {"type": "assert_trap", "action": action("apply", "null"), "text": "uninitialized element"},
        {"type": "assert_trap", "action": action("apply", "2"), "text": "indirect call type mismatch"},
        returned("apply", "16777216", "i32", "0"),
        returned("identity", "18446744073709551615", "funcref", "null"),
        returned("apply", "-1", "i32", "0"),
    ]
    for value in ("7", "null"):
        checks.append({"type": "assert_return", "action": {"type": "invoke", "module": "main",
                       "field": "external_identity", "args": [{"type": "externref", "value": value}]},
                       "expected": [{"type": "externref", "value": value}]})
    # The two reference kinds have the same host ABI but different Wasm
    # types. In particular an externref integer must never become a pointer.
    for field, kind, value in (("apply", "externref", "0"), ("identity", "externref", "null"),
                               ("external_identity", "funcref", "0"), ("external_identity", "funcref", "null")):
        checks.append({"type": "action", "action": {"type": "invoke", "module": "main", "field": field,
                       "args": [{"type": kind, "value": value}]}})
    return checks


def main():
    vm, native, wazero, wat2wasm, fixture = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="pizdos-wat-spec-funcrefs-") as directory:
        work = Path(directory)
        module, compiled, embedded = (work / name for name in ("input.wasm", "native.o", "embedded.o"))
        result = subprocess.run([wat2wasm, fixture, "-o", str(module)], capture_output=True, timeout=30)
        assert result.returncode == 0, result.stderr.decode()
        result = subprocess.run([wazero, "-interpreter", str(module)], capture_output=True, timeout=30)
        assert result.returncode == 0 and result.stdout == b"", result
        for strict in (False, True):
            env = dict(os.environ, W2X_MEMORY_TAIL="strict" if strict else "optimized", W2X_TAIL_PROFILE="")
            result = subprocess.run([native, *(["--strict"] if strict else [])], input=module.read_bytes(), capture_output=True, env=env, timeout=60)
            assert result.returncode == 0, (strict, result.stderr)
            compiled.write_bytes(result.stdout)
            result = subprocess.run([vm, "-o", str(embedded), str(module)], capture_output=True, env=env, timeout=60)
            assert result.returncode == 0 and embedded.read_bytes() == compiled.read_bytes(), (strict, result)
            for obj in (compiled, embedded):
                result = subprocess.run([vm, "run", "--", str(obj)], capture_output=True, env=env, timeout=30)
                assert (result.returncode, result.stdout, result.stderr) == (0, b"", b""), (strict, result)
        # An unused imported function has no addressable adapter. Passing
        # that numeric identity must be skipped before it reaches Wasm.
        unavailable_source = work / "unavailable.wat"
        unavailable_source.write_text('(module (import "spectest" "print" (func)) '
                                      '(func (export "identity") (param funcref) (result funcref) (local.get 0)))')
        unavailable = work / "unavailable.wasm"
        result = subprocess.run([wat2wasm, str(unavailable_source), "-o", str(unavailable)], capture_output=True, timeout=30)
        assert result.returncode == 0, result.stderr
        result = subprocess.run([native, "--strict"], input=unavailable.read_bytes(), capture_output=True, timeout=60)
        assert result.returncode == 0, result.stderr
        compiled.write_bytes(result.stdout)
        result = subprocess.run([vm, "-o", str(embedded), str(unavailable)], capture_output=True,
                                env=dict(os.environ, W2X_MEMORY_TAIL="strict", W2X_TAIL_PROFILE=""), timeout=60)
        assert result.returncode == 0 and embedded.read_bytes() == compiled.read_bytes(), result
        checks = commands(module.name)
        checks.append({"type": "module", "name": "unavailable", "filename": unavailable.name})
        for value in ("0", "null"):
            checks.append({"type": "assert_return", "action": {"type": "invoke", "module": "unavailable",
                           "field": "identity", "args": [{"type": "funcref", "value": value}]},
                           "expected": [{"type": "funcref", "value": "null"}]})
        document = work / "commands.json"
        document.write_text(json.dumps({"commands": checks}))
        result = subprocess.run([vm, "spec", str(document)], capture_output=True, timeout=30)
        assert (result.returncode, result.stdout, result.stderr) == (0, b"RESULT 14 0 8\n", b""), result
    print("WAT spec funcrefs: numeric function identities, nulls and signature traps agree with both AOTs; invalid indices are skipped")


if __name__ == "__main__":
    main()
