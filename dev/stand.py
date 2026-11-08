#!/usr/bin/env python3
"""The stand: native (A), our AOT (C), and bounds-checked wasm2c (R).

    sh dev/stand/build.sh
    dev/stand.py [--runs N] [--only aot,im,pdf,djvu,svg,magic] [--json FILE]

Compiler workloads measure end-to-end wall and CPU time. Decode workloads
measure work inside each driver, excluding startup, linking, file I/O and
output hashing. Small workloads repeat in the same instance. C and R must
agree exactly; native must agree on status, dimensions and page count, but
its pixels may differ with libm and floating-point arithmetic.

The original positional form is accepted for compiler workloads alone:
    stand.py [--runs N] <aot-native> <wasm2o> <stand> <input.wasm>...
"""
import argparse
import hashlib
import json
import math
import os
import platform
import re
import resource
import statistics
import subprocess
import tarfile
import tempfile
import time
from pathlib import Path


KINDS = ("im", "pdf", "djvu", "svg", "magic")
CORPUS_SHA = "0d6a8975159dd12a0a8d6bee270659791780bef806151790054205326c6d2685"


def environment():
    return dict(os.environ, GOMAXPROCS="1", MAGICK_THREAD_LIMIT="1", OMP_NUM_THREADS="1")


def compile_run(command, wasm, out, streams=False):
    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    start = time.perf_counter()
    if streams:
        with open(wasm, "rb") as inp, open(out, "wb") as output:
            subprocess.run(command, stdin=inp, stdout=output, check=True, env=environment())
    else:
        subprocess.run([*command, "-o", str(out), str(wasm)], check=True, env=environment())
    wall = time.perf_counter() - start
    after = resource.getrusage(resource.RUSAGE_CHILDREN)

    return wall, after.ru_utime - before.ru_utime + after.ru_stime - before.ru_stime


def compile_work(native, wasm2o, stand, inputs, runs, work):
    rows = []

    for wasm in inputs:
        expected = work / "want.o"
        compile_run([str(native)], wasm, expected, streams=True)
        want = expected.read_bytes()
        commands = {"A": ([str(native)], True), "C": ([str(wasm2o)], False), "R": ([str(stand)], True)}
        samples = {name: [] for name in commands}

        for run in range(runs):
            order = list(commands)
            order = order[run % 3:] + order[:run % 3]

            for name in order:
                out = work / (name + ".o")
                command, streams = commands[name]
                samples[name].append(compile_run(command, wasm, out, streams))

                if out.read_bytes() != want:
                    raise SystemExit(f"{name} compiles {wasm} to other bytes than native C++ AOT")

        wall = {name: statistics.median(t[0] for t in values) for name, values in samples.items()}
        cpu = {name: statistics.median(t[1] for t in values) for name, values in samples.items()}
        print(f"aot/{Path(wasm).name:16s} A {wall['A']:7.3f}s  C {wall['C']:7.3f}s  R {wall['R']:7.3f}s"
              f"  C/R {wall['C'] / wall['R']:.3f}  C/A {wall['C'] / wall['A']:.3f}  identical", flush=True)
        rows.append({"work": "aot/" + Path(wasm).name, "wall": wall, "cpu": cpu, "samples": samples, "identical": True})

    return rows


def unpack_corpus(archive, work):
    with archive.open("rb") as f:
        digest = hashlib.file_digest(f, "sha256").hexdigest()

    if digest != CORPUS_SHA:
        raise SystemExit(f"{archive}: sha256 {digest}, expected {CORPUS_SHA}")

    with tarfile.open(archive) as tf:
        tf.extractall(work, filter="data")

    root = work / "decode-corpus-1"

    for row in (root / "SHA256SUMS").read_text().splitlines():
        want, name = row.split("  ", 1)

        with (root / name).open("rb") as f:
            actual = hashlib.file_digest(f, "sha256").hexdigest()

        if actual != want:
            raise SystemExit(f"corpus: bad digest of {name}")

    return root


def decode_run(command, repeat, kind, files, count=None):
    try:
        r = subprocess.run([*command, "--repeat", str(repeat), kind, *map(str, files)],
                           env=environment(), capture_output=True, text=True, check=True)
    except subprocess.CalledProcessError as error:
        raise SystemExit(f"{kind}: {command[0]} exited {error.returncode}\n{error.stderr}") from None
    times = re.findall(r"^time ([0-9.]+)$", r.stderr, re.M)

    if len(times) != 1 or len(r.stdout.splitlines()) != (len(files) if count is None else count):
        raise SystemExit(f"bad driver output: {command}\n{r.stdout}\n{r.stderr}")

    return float(times[0]), r.stdout.splitlines()


def shape(kind, row):
    if kind == "magic" or row.endswith(" fail") or row.endswith(" trap"):
        return row

    return row.rsplit(" ", 1)[0]


def decode_work(build, wasm2o, kinds, runs, minimum, work):
    root = unpack_corpus(build / "stand/decode-corpus-1.tgz", work)
    rows = []

    for kind in kinds:
        folder = build / "stand/decode" / kind
        files = sorted(p for p in (root.rglob("*") if kind == "magic" else (root / kind).iterdir())
                       if p.is_file() and p.name not in ("MANIFEST", "SHA256SUMS"))
        commands = {"A": [str(folder / "native")], "C": [str(wasm2o), "stand"], "R": [str(folder / "reference")]}
        # The AOT command has its object after KIND, unlike the C drivers.
        reference_time, want = decode_run(commands["R"], 1, kind, files)
        repeat = 1

        while reference_time < minimum and repeat < 10000:
            repeat = min(10000, max(repeat + 1, math.ceil(repeat * minimum / max(reference_time, 0.000001) * 1.1)))
            reference_time, got = decode_run(commands["R"], repeat, kind, files)

            if got != want:
                raise SystemExit(f"{kind}: reference results changed during calibration")
        samples = {name: [] for name in commands}
        outputs = {}

        for run in range(runs):
            order = list(commands)
            order = order[run % 3:] + order[:run % 3]

            for name in order:
                inputs = [folder / "module.o", *files] if name == "C" else files
                elapsed, got = decode_run(commands[name], repeat, kind, inputs, len(files))

                if name in outputs and got != outputs[name]:
                    raise SystemExit(f"{kind}: {name}'s results changed between runs")

                outputs[name] = got
                samples[name].append(elapsed / repeat)

                if name in ("C", "R") and got != want:
                    diff = [f"  want {x}\n  got  {y}" for x, y in zip(want, got) if x != y]
                    raise SystemExit(f"{kind}: {name} disagrees with wasm2c:\n" + "\n".join(diff[:8]))

        mismatches = [f"{a} != {r}" for a, r in zip(outputs["A"], want) if shape(kind, a) != shape(kind, r)]

        if mismatches:
            raise SystemExit(f"{kind}: native does different work:\n" + "\n".join(mismatches[:8]))

        native_same = sum(a == r for a, r in zip(outputs["A"], want))
        medians = {name: statistics.median(values) for name, values in samples.items()}
        print(f"decode/{kind:12s} A {medians['A']:7.3f}s  C {medians['C']:7.3f}s  R {medians['R']:7.3f}s"
              f"  C/R {medians['C'] / medians['R']:.3f}  C/A {medians['C'] / medians['A']:.3f}"
              f"  {len(files)} files x{repeat}, C=R, native exact {native_same}/{len(files)}", flush=True)
        rows.append({"work": "decode/" + kind, "files": len(files), "repeat": repeat, "seconds": medians,
                     "samples": samples, "outputs": outputs, "native_exact": native_same, "aot_reference_identical": True})

    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("paths", nargs="*")
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--build-dir", type=Path, default=Path(".build"))
    parser.add_argument("--only", default="aot," + ",".join(KINDS))
    parser.add_argument("--min-time", type=float, default=0.25, help="minimum reference time per decode sample")
    parser.add_argument("--json", type=Path, help="write samples and per-file results")
    args = parser.parse_args()
    selected = args.only.split(",")

    if args.runs < 1 or not math.isfinite(args.min_time) or args.min_time <= 0:
        parser.error("runs and min-time must be positive")

    if set(selected) - {"aot", *KINDS}:
        parser.error("unknown workload in --only")

    build = args.build_dir.resolve()
    wasm2o = build / "bin/wasm2o/wasm2o"
    rows = []
    print("A=native C=wasm2o R=wasm2c; GOMAXPROCS=1, MAGICK/OMP threads=1; decode excludes startup, I/O and pixel hashing", flush=True)

    with tempfile.TemporaryDirectory(prefix="stand-") as tmp:
        work = Path(tmp)

        if args.paths:
            if len(args.paths) < 4:
                parser.error("positional form needs aot-native, wasm2o, reference and input.wasm")

            native, compiler, reference, *inputs = (Path(p).absolute() for p in args.paths)
            rows.extend(compile_work(native, compiler, reference, inputs, args.runs, work))
        else:
            if "aot" in selected:
                rows.extend(compile_work(build / "bin/aot/aot", wasm2o, build / "stand/aot",
                                         [build / "bin/busybox/busybox.wasm", build / "bin/aot/aot.wasm"], args.runs, work))

            kinds = [kind for kind in KINDS if kind in selected]

            if kinds:
                rows.extend(decode_work(build, wasm2o, kinds, args.runs, args.min_time, work))

    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps({"platform": platform.platform(), "runs": args.runs, "environment": {k: environment()[k] for k in ("GOMAXPROCS", "MAGICK_THREAD_LIMIT", "OMP_NUM_THREADS")},
                                         "corpus_sha256": CORPUS_SHA, "rows": rows}, indent=2) + "\n")


if __name__ == "__main__":
    main()
