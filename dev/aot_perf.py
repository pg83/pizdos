#!/usr/bin/env python3
"""Compare native C++ and embedded AOT on the main corpus, checking every ELF byte.

CPU is user+system time from wait4, not elapsed time. Runs alternate compiler
order; each invocation is a fresh process. RSS is measured by a small C parent
to avoid inheriting Python's high-water mark. The report compares median CPU
and peak RSS on every input. Optional gates limit embedded/native ratios.
"""
import argparse
import hashlib
import json
import os
import shutil
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
    "compiler": ".build/bin/aot/aot.wasm",
}


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            h.update(chunk)
    return h.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", type=Path, default=ROOT / ".build/bin/aot/aot")
    parser.add_argument("--embedded", type=Path, default=ROOT / ".build/bin/wasm2o/wasm2o")
    parser.add_argument("--measure", type=Path, default=ROOT / ".build/dev/aot_measure")
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--only", choices=INPUTS, nargs="+")
    parser.add_argument("--json", type=Path, required=True)
    parser.add_argument("--objects", type=Path, help="keep verified outputs for generated-code benchmarks")
    parser.add_argument("--compiler-input", type=Path, help="freeze the compiler wasm input across revisions")
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--max-cpu-ratio", type=float)
    parser.add_argument("--max-rss-ratio", type=float)
    args = parser.parse_args()
    if args.runs < 1:
        parser.error("--runs must be positive")

    binaries = {"native": args.native.resolve(), "embedded": args.embedded.resolve()}
    report = {
        "method": "median user+system CPU; worst embedded RSS / median native RSS; fresh processes; alternating order",
        "binaries": {key: {"path": str(path), "sha256": digest(path)} for key, path in binaries.items()},
        "runs": args.runs,
        "full_corpus": args.only is None or set(args.only) == set(INPUTS),
        "targets": {"cpu_ratio": args.max_cpu_ratio, "rss_ratio": args.max_rss_ratio},
        "inputs": [],
    }
    env = dict(os.environ)
    env.update({key: "" for key in ("W2X_DUMP", "W2X_TRACE", "W2X_CHECK", "W2X_PROFILE", "W2X_TAIL_PROFILE", "W2X_OPT_FUNCS")})
    env["W2X_MEMORY_TAIL"] = "optimized"
    args.json.parent.mkdir(parents=True, exist_ok=True)
    if args.objects:
        args.objects.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="aot-perf-") as work:
        work = Path(work)
        for name in args.only or INPUTS:
            source = args.compiler_input.resolve() if name == "compiler" and args.compiler_input else ROOT / INPUTS[name]
            row = {"name": name, "input_sha256": digest(source), "native": [], "embedded": []}
            expected = None
            for run in range(args.runs):
                for compiler in (("native", "embedded") if run % 2 == 0 else ("embedded", "native")):
                    output = work / f"{compiler}.o"
                    command = [str(binaries[compiler])]
                    if compiler == "embedded":
                        command.extend(["-o", str(output), str(source)])
                    with source.open("rb") as inp, output.open("wb") as out:
                        result = subprocess.run([str(args.measure.resolve()), *command], stdin=inp, stdout=out if compiler == "native" else subprocess.DEVNULL, stderr=subprocess.PIPE, env=env)
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
            row["cpu_ratio"] = median("embedded", "cpu") / median("native", "cpu")
            row["rss_ratio"] = max(item["rss_kib"] for item in row["embedded"]) / median("native", "rss_kib")
            row["passed"] = (args.max_cpu_ratio is None or row["cpu_ratio"] <= args.max_cpu_ratio) and (args.max_rss_ratio is None or row["rss_ratio"] <= args.max_rss_ratio)
            row["elf_sha256"] = expected
            if args.objects:
                shutil.copyfile(output, args.objects / f"{name}.o")
            report["inputs"].append(row)
            args.json.write_text(json.dumps(report, indent=2) + "\n")
            print(f"{name}: CPU native {median('native', 'cpu'):.3f}s embedded {median('embedded', 'cpu'):.3f}s ({row['cpu_ratio']:.3f}x); RSS native {median('native', 'rss_kib') / 1024:.1f} embedded {max(item['rss_kib'] for item in row['embedded']) / 1024:.1f} MiB ({row['rss_ratio']:.3f}x); {'PASS' if row['passed'] else 'FAIL'}", flush=True)
    if args.check and not all(row["passed"] for row in report["inputs"]):
        raise SystemExit(1)


if __name__ == "__main__":
    main()
