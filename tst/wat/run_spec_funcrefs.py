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

    return [
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
        document = work / "commands.json"
        document.write_text(json.dumps({"commands": commands(module.name)}))
        result = subprocess.run([vm, "spec", str(document)], capture_output=True, timeout=30)
        assert (result.returncode, result.stdout, result.stderr) == (0, b"RESULT 10 0 3\n", b""), result
    print("WAT spec funcrefs: numeric function identities, nulls and signature traps agree with both AOTs; invalid indices are skipped")


if __name__ == "__main__":
    main()
