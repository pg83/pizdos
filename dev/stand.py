#!/usr/bin/env python3
"""The stand: native (A) against our AOT (C).

    sh dev/stand/build.sh
    dev/stand.py [--runs N] [--only aot,im,pdf,djvu,svg,magic,py,py/NAME] [--json FILE]

Compiler workloads measure end-to-end wall and CPU time. Decode workloads
measure work inside each driver, excluding startup, linking, file I/O and
output hashing. Small workloads repeat in the same instance. The compiler's
outputs must be identical; decode must agree on status, dimensions and
page count, and the report counts the files whose pixels match exactly:
native pixels may differ with libm and floating-point arithmetic. CPython's
workloads are the benchmarks of dev/stand/pybench.py, pure Python timed
inside the interpreter, the native CPython and python.wasm, which the kernel
compiles as it starts, each in a root with the same standard library zip;
their digests of the results must be equal.
The suite counts as one workload in the geometric mean, by its own.

The original positional form is accepted for compiler workloads alone:
    stand.py [--runs N] <aot-native> <vm> <input.wasm>...
"""
import argparse
import hashlib
import json
import math
import os
import platform
import re
import resource
import shutil
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


def compile_work(native, vm, inputs, runs, work):
    rows = []

    for wasm in inputs:
        expected = work / "want.o"
        compile_run([str(native)], wasm, expected, streams=True)
        want = expected.read_bytes()
        commands = {"A": ([str(native)], True), "C": ([str(vm)], False)}
        samples = {name: [] for name in commands}

        for run in range(runs):
            order = list(commands)
            order = order[run % 2:] + order[:run % 2]

            for name in order:
                out = work / (name + ".o")
                command, streams = commands[name]
                samples[name].append(compile_run(command, wasm, out, streams))

                if out.read_bytes() != want:
                    raise SystemExit(f"{name} compiles {wasm} to other bytes than native C++ AOT")

        wall = {name: statistics.median(t[0] for t in values) for name, values in samples.items()}
        cpu = {name: statistics.median(t[1] for t in values) for name, values in samples.items()}
        print(f"aot/{Path(wasm).name:16s} A {wall['A']:7.3f}s  C {wall['C']:7.3f}s  C/A {wall['C'] / wall['A']:.3f}  identical", flush=True)
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


def decode_work(build, vm, kinds, runs, minimum, work, objects=None):
    root = unpack_corpus(build / "stand/decode-corpus-1.tgz", work)
    rows = []

    for kind in kinds:
        folder = build / "stand/decode" / kind
        obj = objects / f"{kind}.o" if objects else folder / "module.o"
        files = sorted(p for p in (root.rglob("*") if kind == "magic" else (root / kind).iterdir())
                       if p.is_file() and p.name not in ("MANIFEST", "SHA256SUMS"))
        commands = {"A": [str(folder / "native")], "C": [str(vm), "stand"]}
        # The AOT command has its object after KIND, unlike the native driver.
        native_time, want = decode_run(commands["A"], 1, kind, files)
        repeat = 1

        while native_time < minimum and repeat < 10000:
            repeat = min(10000, max(repeat + 1, math.ceil(repeat * minimum / max(native_time, 0.000001) * 1.1)))
            native_time, got = decode_run(commands["A"], repeat, kind, files)

            if got != want:
                raise SystemExit(f"{kind}: native results changed during calibration")
        samples = {name: [] for name in commands}
        outputs = {}

        for run in range(runs):
            order = list(commands)
            order = order[run % 2:] + order[:run % 2]

            for name in order:
                inputs = [obj, *files] if name == "C" else files
                elapsed, got = decode_run(commands[name], repeat, kind, inputs, len(files))

                if name in outputs and got != outputs[name]:
                    raise SystemExit(f"{kind}: {name}'s results changed between runs")

                outputs[name] = got
                samples[name].append(elapsed / repeat)

        mismatches = [f"{c} != {a}" for c, a in zip(outputs["C"], want) if shape(kind, c) != shape(kind, a)]

        if mismatches:
            raise SystemExit(f"{kind}: vm does different work than native:\n" + "\n".join(mismatches[:8]))

        exact = sum(c == a for c, a in zip(outputs["C"], want))
        medians = {name: statistics.median(values) for name, values in samples.items()}
        print(f"decode/{kind:12s} A {medians['A']:7.3f}s  C {medians['C']:7.3f}s  C/A {medians['C'] / medians['A']:.3f}"
              f"  {len(files)} files x{repeat}, exact {exact}/{len(files)}", flush=True)
        rows.append({"work": "decode/" + kind, "files": len(files), "repeat": repeat, "seconds": medians,
                     "samples": samples, "outputs": outputs, "exact": exact,
                     "object_sha256": hashlib.sha256(obj.read_bytes()).hexdigest()})

    return rows


def python_root(root, interpreter, build):
    """A root of the Python workload: the interpreter as /usr/bin/python3,
    the standard library zip and the benchmark."""
    (root / "usr/bin").mkdir(parents=True)
    (root / "usr/lib/python3.14/lib-dynload").mkdir(parents=True)
    shutil.copy(interpreter, root / "usr/bin/python3")
    (root / "usr/bin/python3").chmod(0o755)
    shutil.copy(build / "bin/python/python314.zip", root / "usr/lib/python314.zip")
    shutil.copy(Path(__file__).resolve().parent / "stand" / "pybench.py", root / "pybench.py")

    return root


def python_run(command, repeat):
    env = {"PYTHONHASHSEED": "0", **{k: environment()[k] for k in ("GOMAXPROCS", "MAGICK_THREAD_LIMIT", "OMP_NUM_THREADS")}}

    try:
        r = subprocess.run([*command, str(repeat)], env=env, capture_output=True, text=True, check=True)
    except subprocess.CalledProcessError as error:
        raise SystemExit(f"py: {command[0]} exited {error.returncode}\n{error.stderr}") from None

    times = re.findall(r"^time ([0-9.]+)$", r.stderr, re.M)

    if len(times) != 1 or len(r.stdout.split()) != 1:
        raise SystemExit(f"bad benchmark output: {command}\n{r.stdout}\n{r.stderr}")

    return float(times[0]), r.stdout.strip()


def python_work(build, vm, runs, minimum, work, only=None):
    """Every benchmark of the suite: its repeat calibrated on native to the
    minimum time, A and C interleaved, their digests equal."""
    native = python_root(work / "python-A", build / "stand/python/native", build)
    aot = python_root(work / "python-C", build / "bin/python/python.wasm", build)
    names = subprocess.run([str(native / "usr/bin/python3"), "-S", str(native / "pybench.py"), "--list"],
                           env={"PYTHONHASHSEED": "0"}, capture_output=True, text=True, check=True).stdout.split()
    rows = []

    for bench in names:
        if only and bench not in only:
            continue

        commands = {
            "A": [str(native / "usr/bin/python3"), "-S", str(native / "pybench.py"), bench],
            "C": [str(vm), "run", "--root", str(aot), "-a", "/usr/bin/python3", "--", str(aot / "usr/bin/python3"), "-S", "/pybench.py", bench],
        }
        native_time, _ = python_run(commands["A"], 1)
        repeat = max(1, math.ceil(minimum / max(native_time, 0.000001)))
        samples = {name: [] for name in commands}
        outputs = {}

        for run in range(runs):
            order = list(commands)
            order = order[run % 2:] + order[:run % 2]

            for name in order:
                elapsed, got = python_run(commands[name], repeat)

                if outputs.setdefault(name, got) != got:
                    raise SystemExit(f"py/{bench}: {name}'s results changed between runs")

                samples[name].append(elapsed / repeat)

        if outputs["A"] != outputs["C"]:
            raise SystemExit(f"py/{bench}: vm computes {outputs['C']}, native {outputs['A']}")

        medians = {name: statistics.median(values) for name, values in samples.items()}
        print(f"py/{bench:16s} A {medians['A']:7.4f}s  C {medians['C']:7.4f}s  C/A {medians['C'] / medians['A']:.3f}  x{repeat}, digest equal",
              flush=True)
        rows.append({"work": "py/" + bench, "repeat": repeat, "seconds": medians, "samples": samples, "digest": outputs["A"]})

    if rows:
        print(f"py geomean C/A {geomean(rows):.3f}", flush=True)

    return rows


def ratio(row):
    seconds = row.get("wall") or row["seconds"]

    return seconds["C"] / seconds["A"]


def geomean(rows):
    return math.exp(sum(math.log(ratio(row)) for row in rows) / len(rows))


def corpus_geomean(rows):
    """The geometric mean over the workloads, CPython's suite one of them by
    its own geometric mean."""
    py = [row for row in rows if row["work"].startswith("py/")]
    ratios = [ratio(row) for row in rows if not row["work"].startswith("py/")]

    if py:
        ratios.append(geomean(py))

    return math.exp(sum(map(math.log, ratios)) / len(ratios))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("paths", nargs="*")
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--build-dir", type=Path, default=Path(".build"))
    parser.add_argument("--objects", type=Path, help="use saved KIND.o files for generated-code comparisons")
    parser.add_argument("--only", default="aot," + ",".join(KINDS) + ",py")
    parser.add_argument("--min-time", type=float, default=0.25, help="minimum native time per decode sample")
    parser.add_argument("--json", type=Path, help="write samples and per-file results")
    args = parser.parse_args()
    selected = args.only.split(",")

    if args.runs < 1 or not math.isfinite(args.min_time) or args.min_time <= 0:
        parser.error("runs and min-time must be positive")

    if {s for s in selected if not s.startswith("py/")} - {"aot", "py", *KINDS}:
        parser.error("unknown workload in --only")

    build = args.build_dir.resolve()
    vm = build / "bin/vm/vm"
    rows = []
    print("A=native C=vm; GOMAXPROCS=1, MAGICK/OMP threads=1; decode excludes startup, I/O and pixel hashing", flush=True)

    with tempfile.TemporaryDirectory(prefix="stand-") as tmp:
        work = Path(tmp)

        if args.paths:
            if len(args.paths) < 3:
                parser.error("positional form needs aot-native, vm and input.wasm")

            native, compiler, *inputs = (Path(p).absolute() for p in args.paths)
            rows.extend(compile_work(native, compiler, inputs, args.runs, work))
        else:
            if "aot" in selected:
                rows.extend(compile_work(build / "bin/aot/aot", vm,
                                         [build / "bin/busybox/busybox.wasm", build / "bin/aot/aot.wasm"], args.runs, work))

            kinds = [kind for kind in KINDS if kind in selected]

            if kinds:
                rows.extend(decode_work(build, vm, kinds, args.runs, args.min_time, work, args.objects))

            benches = {s[len("py/"):] for s in selected if s.startswith("py/")}

            if "py" in selected or benches:
                rows.extend(python_work(build, vm, args.runs, args.min_time, work, benches if "py" not in selected else None))

    if rows:
        print(f"geomean C/A {corpus_geomean(rows):.3f}", flush=True)

    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps({"platform": platform.platform(), "runs": args.runs, "environment": {k: environment()[k] for k in ("GOMAXPROCS", "MAGICK_THREAD_LIMIT", "OMP_NUM_THREADS")},
                                         "corpus_sha256": CORPUS_SHA, "rows": rows}, indent=2) + "\n")


if __name__ == "__main__":
    main()
