#!/usr/bin/env python3
"""Compare native compilers on the main corpus, checking every ELF byte.

CPU is user+system time from wait4, not elapsed time. Runs alternate compiler
order; each invocation is a fresh process. RSS is measured by a small C parent
to avoid inheriting Python's high-water mark. The gate compares median CPU
and worst observed C++ RSS against median Go RSS, separately on every input.
"""
import argparse
import hashlib
import json
import os
import statistics
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
INPUTS = {
    "busybox": ".build/bin/busybox/busybox.wasm",
    "im": ".build/stand/decode/im/input.wasm",
    "pdf": ".build/stand/decode/pdf/input.wasm",
    "djvu": ".build/stand/decode/djvu/input.wasm",
    "svg": ".build/stand/decode/svg/input.wasm",
    "magic": ".build/stand/decode/magic/input.wasm",
    "compiler": ".build/bin/wasm2o/wasm2o.wasm",
}


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            h.update(chunk)
    return h.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--go", type=Path, default=ROOT / ".build/bin/wasm2o/wasm2o")
    parser.add_argument("--cpp", type=Path, default=ROOT / ".build/bin/aot/aot")
    parser.add_argument("--measure", type=Path, default=ROOT / ".build/dev/aot_measure")
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--only", choices=INPUTS, nargs="+")
    parser.add_argument("--json", type=Path, required=True)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    if args.runs < 1:
        parser.error("--runs must be positive")

    binaries = {"go": args.go.resolve(), "cpp": args.cpp.resolve()}
    report = {
        "method": "median user+system CPU; worst C++ RSS / median Go RSS; fresh processes; alternating order",
        "binaries": {key: {"path": str(path), "sha256": digest(path)} for key, path in binaries.items()},
        "runs": args.runs,
        "full_corpus": args.only is None or set(args.only) == set(INPUTS),
        "targets": {"cpu_ratio": 0.5, "rss_ratio": 1.0},
        "inputs": [],
    }
    env = dict(os.environ)
    env.update({key: "" for key in ("W2X_DUMP", "W2X_TRACE", "W2X_CHECK", "W2X_PROFILE", "W2X_TAIL_PROFILE", "W2X_OPT_FUNCS")})
    env["W2X_MEMORY_TAIL"] = "optimized"
    args.json.parent.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="aot-perf-") as work:
        work = Path(work)
        for name in args.only or INPUTS:
            source = ROOT / INPUTS[name]
            row = {"name": name, "input_sha256": digest(source), "go": [], "cpp": []}
            expected = None
            for run in range(args.runs):
                for compiler in (("go", "cpp") if run % 2 == 0 else ("cpp", "go")):
                    output = work / f"{compiler}.o"
                    command = [str(binaries[compiler])]
                    if compiler == "go":
                        command.extend(["-o", str(output), str(source)])
                    with source.open("rb") as inp, output.open("wb") as out:
                        result = subprocess.run([str(args.measure.resolve()), *command], stdin=inp, stdout=out if compiler == "cpp" else subprocess.DEVNULL, stderr=subprocess.PIPE, env=env)
                    if result.returncode:
                        raise RuntimeError(result.stderr.decode(errors="replace"))
                    lines = result.stderr.decode().splitlines()
                    measurement = json.loads(next(line[len("AOT_MEASURE "):] for line in lines if line.startswith("AOT_MEASURE ")))
                    measurement["cpu"] = measurement["user"] + measurement["system"]
                    actual = digest(output)
                    if expected is None:
                        expected = actual
                    if expected != actual:
                        raise RuntimeError(f"{name}: {compiler} changed ELF bytes")
                    row[compiler].append(measurement)
            median = lambda compiler, key: statistics.median(item[key] for item in row[compiler])
            row["cpu_ratio"] = median("cpp", "cpu") / median("go", "cpu")
            row["rss_ratio"] = max(item["rss_kib"] for item in row["cpp"]) / median("go", "rss_kib")
            row["passed"] = row["cpu_ratio"] <= 0.5 and row["rss_ratio"] <= 1.0
            row["elf_sha256"] = expected
            report["inputs"].append(row)
            args.json.write_text(json.dumps(report, indent=2) + "\n")
            print(f"{name}: CPU Go {median('go', 'cpu'):.3f}s C++ {median('cpp', 'cpu'):.3f}s ({row['cpu_ratio']:.3f}x); RSS Go {median('go', 'rss_kib') / 1024:.1f} C++ {max(item['rss_kib'] for item in row['cpp']) / 1024:.1f} MiB ({row['rss_ratio']:.3f}x); {'PASS' if row['passed'] else 'FAIL'}", flush=True)
    if args.check and not all(row["passed"] for row in report["inputs"]):
        raise SystemExit(1)


if __name__ == "__main__":
    main()
