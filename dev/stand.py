#!/usr/bin/env python3
"""Times wasm2o's code against wasm2c's: wasm2o.wasm compiling each input,
as wasm2o compiled it under `wasm2o run` (C) and as `./build stand` made it
(R), runs interleaved. Both must make the bytes the native wasm2o makes.

    stand.py [--runs N] <wasm2o> <wasm2o.o> <stand> <input.wasm>...

Prints, per input, the median wall and CPU time of each and C over R.
"""
import os
import resource
import statistics
import subprocess
import sys
import tempfile
import time


def run(command, wasm, out):
    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    start = time.perf_counter()
    subprocess.run([*command, "-o", out, wasm], check=True)
    wall = time.perf_counter() - start
    after = resource.getrusage(resource.RUSAGE_CHILDREN)

    return wall, after.ru_utime - before.ru_utime + after.ru_stime - before.ru_stime


def main():
    args = sys.argv[1:]
    runs = 5

    if args[:1] == ["--runs"]:
        runs = int(args[1])
        args = args[2:]

    wasm2o, wasm2o_o, stand, *inputs = args

    with tempfile.TemporaryDirectory(prefix="stand-") as work:
        for wasm in inputs:
            subprocess.run([wasm2o, "-o", f"{work}/A.o", wasm], check=True)
            want = open(f"{work}/A.o", "rb").read()
            commands = {"C": [wasm2o, "run", wasm2o_o, "--"], "R": [stand]}
            times = {name: [] for name in commands}

            for _ in range(runs):
                for name, command in commands.items():
                    times[name].append(run(command, wasm, f"{work}/{name}.o"))

                    if open(f"{work}/{name}.o", "rb").read() != want:
                        raise SystemExit(f"{name} compiles {wasm} to other bytes than wasm2o")

            wall = {name: statistics.median(t[0] for t in times[name]) for name in commands}
            cpu = {name: statistics.median(t[1] for t in times[name]) for name in commands}
            print(f"{os.path.basename(wasm):16s} C {wall['C']:7.3f}s cpu {cpu['C']:7.3f}s   R {wall['R']:7.3f}s cpu {cpu['R']:7.3f}s"
                  f"   C/R {wall['C'] / wall['R']:.3f} cpu {cpu['C'] / cpu['R']:.3f}")


if __name__ == "__main__":
    main()
