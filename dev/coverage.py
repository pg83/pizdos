#!/usr/bin/env python3
"""Run observable-behaviour tests and report AOT/VM coverage separately.

    coverage.py <output-directory> <suite-json>
    coverage.py --check <summary.json> [minimum-percent]

Clang/LLVM measures native aot.cpp and the embedded Wasm reactor;
go build -cover measures the VM executable. The AOT goal counts source
lines other than standalone block braces; raw LLVM totals remain alongside
that metric and in the unmodified LCOV files.
Per-process profiles survive children that deliberately replace their
environment. All tests invoke executables, never internal compiler APIs.
"""
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time

import coverage_wasm


def output(command, **options):
    return subprocess.check_output(list(map(str, command)), text=True, **options)


def wrapper(path, binary, variables):
    variables = {key: str(value) for key, value in variables.items()}
    path.write_text(f"#!{sys.executable}\nimport os, sys\n"
                    f"os.environ.update({variables!r})\n"
                    f"os.execv({binary!r}, [{binary!r}, *sys.argv[1:]])\n")
    path.chmod(0o755)


def metric(hit, total):
    return {"hit": hit, "total": total, "percent": 100 * hit / total if total else 0.0}


def executable_lines(lines, source):
    # LLVM maps block delimiters too, including unreachable closing braces
    # after return. Apply the same lexical rule to hit and missed lines;
    # the unfiltered LLVM metric and LCOV data are retained for comparison.
    return {number: count for number, count in lines.items() if source[number - 1].strip() not in ("{", "}")}


def aot_report(suite, raw, destination, *, chaos=False):
    profiles = sorted(raw.glob("*.profraw"))
    report_name = "aot-chaos.json" if chaos else "aot.json"
    stem = "aot-native-chaos" if chaos else "aot-native"
    if not profiles:
        if chaos:
            (destination / report_name).write_text('{"files": []}\n')
            (destination / (stem + ".profdata")).write_bytes(b"")
            (destination / (stem + ".lcov")).write_text("")
            return {}, {}
        raise RuntimeError("native AOT produced no LLVM profiles")
    indexed = destination / (stem + ".profdata")
    subprocess.run([suite["llvm_profdata"], "merge", "-sparse", *map(str, profiles), "-o", str(indexed)], check=True)
    source = Path(suite["source_root"], "bin/aot/aot.cpp").resolve()
    options = [suite["aot_chaos" if chaos else "aot"], "-instr-profile=" + str(indexed), str(source)]
    (destination / report_name).write_text(output([suite["llvm_cov"], "export", *options]))
    trace = output([suite["llvm_cov"], "export", "-format=lcov", *options])
    (destination / (stem + ".lcov")).write_text(trace)
    lines, branches = {}, {}
    selected = False
    for row in trace.splitlines():
        if row.startswith("SF:"):
            selected = Path(row[3:]).resolve() == source
        elif selected and row.startswith("DA:"):
            number, count, *_ = row[3:].split(",")
            number, count = int(number), int(count)
            lines[number] = lines.get(number, 0) + count
        elif selected and row.startswith("BRDA:"):
            number, block, branch, count = row[5:].split(",")
            key = int(number), block, branch
            branches[key] = branches.get(key, 0) + (0 if count == "-" else int(count))
    if not lines:
        raise RuntimeError("LLVM profile contains no native aot.cpp lines")
    return lines, branches


def codecov_aot(path, lines):
    # Keep raw LCOV intact alongside this line report. Codecov and the local
    # floor use the same brace-only filter and one record per source line.
    rows = ["SF:bin/aot/aot.cpp"]
    rows.extend(f"DA:{number},{count}" for number, count in sorted(lines.items()))
    rows.extend((f"LF:{len(lines)}", f"LH:{sum(count > 0 for count in lines.values())}", "end_of_record"))
    path.write_text("\n".join(rows) + "\n")


def vm_report(suite, raw, destination):
    profile = destination / "vm.out"
    subprocess.run([suite["go"], "tool", "covdata", "textfmt", "-i=" + str(raw), "-o=" + str(profile)], check=True)
    files, missing, blocks = {}, [], {}
    for line in profile.read_text().splitlines()[1:]:
        location, statements, count = line.rsplit(" ", 2)
        name, coordinates = location.rsplit(":", 1)
        if not name.startswith("pizdos/vm/"):
            continue
        # This file is staged from dev/ only into measurement builds.
        if name == "pizdos/vm/coverage_collect.go":
            continue
        name = "bin/vm/" + name[len("pizdos/vm/"):]
        statements, count = int(statements), int(count)
        key = name, coordinates, statements
        blocks[key] = blocks.get(key, 0) + count
    for (name, coordinates, statements), count in blocks.items():
        hit, total = files.get(name, (0, 0))
        files[name] = (hit + (statements if count else 0), total + statements)
        if not count and statements:
            start = int(coordinates.split(".", 1)[0])
            missing.append((name, start, statements))
    if not files:
        raise RuntimeError("Go profile contains no bin/vm statements")
    rows = ["mode: atomic"]
    rows.extend(f"{name}:{coordinates} {statements} {count}" for (name, coordinates, statements), count in sorted(blocks.items()))
    (destination / "vm-codecov.out").write_text("\n".join(rows) + "\n")
    hit = sum(count[0] for count in files.values())
    total = sum(count[1] for count in files.values())
    return {"statements": metric(hit, total),
            "files": {name: metric(*counts) for name, counts in sorted(files.items())}}, missing


def run(destination, suite):
    destination = Path(destination).resolve()
    destination.mkdir(parents=True, exist_ok=True)
    source_root = Path(suite["source_root"])
    with tempfile.TemporaryDirectory(prefix="pizdos-coverage-") as directory:
        work = Path(directory)
        go_raw, aot_raw, wasm_raw = work / "go", work / "aot", work / "wasm"
        aot_chaos_raw = work / "aot-chaos"
        go_raw.mkdir()
        aot_raw.mkdir()
        aot_chaos_raw.mkdir()
        wasm_raw.mkdir()
        vm, aot = work / "vm", work / "aot-command"
        vm_chaos, aot_chaos = work / "vm-chaos", work / "aot-command-chaos"
        wrapper(vm, suite["vm"], {"GOCOVERDIR": go_raw, "PIZDOS_WASM_COVERDIR": wasm_raw})
        wrapper(aot, suite["aot"], {"LLVM_PROFILE_FILE": aot_raw / "%8m.profraw"})
        wrapper(vm_chaos, suite["vm_chaos"], {"GOCOVERDIR": go_raw, "PIZDOS_WASM_COVERDIR": wasm_raw})
        wrapper(aot_chaos, suite["aot_chaos"], {"LLVM_PROFILE_FILE": aot_chaos_raw / "%8m.profraw"})
        failures = []
        for case in suite["cases"]:
            name = case["name"]
            command = [value.replace("$VM_CHAOS", str(vm_chaos)).replace("$AOT_CHAOS", str(aot_chaos))
                       .replace("$VM", str(vm)).replace("$AOT", str(aot)).replace("$OUT", str(work / name))
                       for value in case["command"]]
            started = time.monotonic()
            print(f"coverage: {name}", flush=True)
            try:
                result = subprocess.run(command, capture_output=True, text=True, timeout=900)
                (destination / (name + ".log")).write_text(result.stdout + result.stderr)
                if result.returncode:
                    failures.append(name)
                    print(f"FAIL {name}: exit {result.returncode}\n{result.stdout[-4000:]}{result.stderr[-4000:]}", flush=True)
            except subprocess.TimeoutExpired:
                failures.append(name)
                print(f"FAIL {name}: timeout", flush=True)
            print(f"coverage: {name} finished in {time.monotonic() - started:.1f}s", flush=True)
        aot_lines, aot_branches = aot_report(suite, aot_raw, destination)
        chaos_lines, chaos_branches = aot_report(suite, aot_chaos_raw, destination, chaos=True)
        for number, count in chaos_lines.items():
            aot_lines[number] = aot_lines.get(number, 0) + count
        source = (source_root / "bin/aot/aot.cpp").read_text().splitlines()
        aot_counts = {"raw_lines": metric(sum(count > 0 for count in aot_lines.values()), len(aot_lines)),
                      "branches": metric(sum(count > 0 for count in aot_branches.values()), len(aot_branches)),
                      "chaos_branches": metric(sum(count > 0 for count in chaos_branches.values()), len(chaos_branches))}
        aot_lines = executable_lines(aot_lines, source)
        aot_counts["lines"] = metric(sum(count > 0 for count in aot_lines.values()), len(aot_lines))
        codecov_aot(destination / "aot-native.info", aot_lines)
        wasm_raw_lines = coverage_wasm.report(suite, wasm_raw, destination)
        aot_counts["wasm_raw_lines"] = metric(sum(count > 0 for count in wasm_raw_lines.values()), len(wasm_raw_lines))
        wasm_lines = executable_lines(wasm_raw_lines, source)
        aot_counts["wasm_lines"] = metric(sum(count > 0 for count in wasm_lines.values()), len(wasm_lines))
        codecov_aot(destination / "aot-wasm.info", wasm_lines)
        vm_counts, vm_missing = vm_report(suite, go_raw, destination)

    summary = {"aot": aot_counts, "vm": vm_counts, "tests": [case["name"] for case in suite["cases"]], "failures": failures}
    (destination / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    rows = []
    for name, counts in (("AOT executable lines (native)", aot_counts["lines"]), ("AOT lines (native raw LLVM)", aot_counts["raw_lines"]),
                         ("AOT executable lines (Wasm)", aot_counts["wasm_lines"]),
                         ("AOT lines (Wasm raw LLVM)", aot_counts["wasm_raw_lines"]),
                         ("AOT branches (normal)", aot_counts["branches"]),
                         ("AOT branches (chaos)", aot_counts["chaos_branches"]),
                         ("VM statements", vm_counts["statements"])):
        rows.append(f"{name}: {counts['hit']}/{counts['total']} = {counts['percent']:.2f}%")
    rows.extend(f"  {name}: {counts['hit']}/{counts['total']} = {counts['percent']:.2f}%"
                for name, counts in vm_counts["files"].items())
    (destination / "summary.txt").write_text("\n".join(rows) + "\n")
    print("\n".join(rows), flush=True)
    sources = {"bin/aot/aot.cpp": (source_root / "bin/aot/aot.cpp").read_text().splitlines()}
    missed = []
    for line, count in sorted(aot_lines.items()):
        if not count:
            missed.append(f"bin/aot/aot.cpp:{line}: {sources['bin/aot/aot.cpp'][line - 1].strip()}")
    for line, count in sorted(wasm_lines.items()):
        if not count:
            missed.append(f"bin/aot/aot.cpp:{line}: [Wasm] {sources['bin/aot/aot.cpp'][line - 1].strip()}")
    for name, line, statements in sorted(vm_missing):
        if name not in sources:
            sources[name] = (source_root / name).read_text().splitlines()
        missed.append(f"{name}:{line}: {statements} statements: {sources[name][line - 1].strip()}")
    (destination / "uncovered.txt").write_text("\n".join(missed) + "\n")
    if failures:
        raise RuntimeError("end-to-end failures: " + ", ".join(failures))


def check(path, minimum):
    summary = json.loads(Path(path).read_text())
    if summary["failures"]:
        raise RuntimeError("the coverage suite has failing tests")
    for name, counts in (("AOT executable lines (native)", summary["aot"]["lines"]),
                         ("AOT executable lines (Wasm)", summary["aot"]["wasm_lines"]), ("VM statements", summary["vm"]["statements"])):
        if counts["percent"] < minimum:
            raise RuntimeError(f"{name}: {counts['percent']:.2f}% < {minimum:.2f}%")


if __name__ == "__main__":
    try:
        if sys.argv[1] == "--check":
            check(sys.argv[2], float(sys.argv[3]) if len(sys.argv) > 3 else 99.0)
        else:
            run(sys.argv[1], json.loads(sys.argv[2]))
    except (RuntimeError, subprocess.CalledProcessError) as error:
        sys.exit(str(error))
