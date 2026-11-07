#!/usr/bin/env python3
"""The WebAssembly spec testsuite through `wasm2o spec`.

    wasm2o_spec.py <wasm2o> <wast2json> <testsuite.tar.gz> <stamp>

Every top-level .wast is converted by wast2json and run by wasm2o, which
compiles and loads its modules and counts its checks. A file that does not
convert is not counted. The test fails when a file crashes or fails more
checks than KNOWN allows, and prints the totals.
"""
import concurrent.futures
import os
import re
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

# Failures of what wasm2o does not implement, by file: table.grow, which
# fails; modules of two memories, skipped, which were to fill memory or a
# table that other modules read.
KNOWN = {
    "linking0.wast": 1,
    "load1.wast": 5,
    "table_grow.wast": 30,
    "table_size.wast": 15,
}


def run(wasm2o, wast2json, wast, work):
    work.mkdir(parents=True)
    r = subprocess.run([wast2json, str(wast), "-o", str(work / "t.json")], capture_output=True, text=True)

    if r.returncode != 0:
        return None

    try:
        r = subprocess.run([wasm2o, "spec", str(work / "t.json")], capture_output=True, text=True, timeout=600)
    except subprocess.TimeoutExpired:
        return "timeout"

    m = re.search(r"^RESULT (\d+) (\d+) (\d+)$", r.stdout, re.M)

    if r.returncode != 0 or m is None:
        return f"crashed ({r.returncode}): " + "\n".join((r.stdout + r.stderr).strip().splitlines()[-5:])

    failures = [line for line in r.stdout.splitlines() if line.startswith("FAIL")]

    return int(m.group(1)), int(m.group(2)), int(m.group(3)), failures


def main():
    wasm2o, wast2json, tarball, stamp = sys.argv[1:]

    if not wast2json or not os.access(wast2json, os.X_OK):
        sys.exit("wast2json is not on PATH: run the build under ix run bld/wabt")

    with tempfile.TemporaryDirectory(prefix="spec-") as tmp:
        tmp = Path(tmp)

        with tarfile.open(tarball) as tf:
            members = [m for m in tf.getmembers() if m.name.count("/") == 1 and m.name.endswith(".wast")]
            tf.extractall(tmp / "src", members=members, filter="data")

        wasts = sorted((tmp / "src").glob("*/*.wast"))
        totals = [0, 0, 0]
        bad = []

        with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count()) as pool:
            jobs = {w.name: pool.submit(run, wasm2o, wast2json, w, tmp / "work" / w.stem) for w in wasts}

            for name, job in jobs.items():
                res = job.result()

                if res is None:
                    continue

                if isinstance(res, str):
                    print(f"{name}: {res}")
                    bad.append(name)

                    continue

                passed, failed, skipped, failures = res
                totals[0] += passed
                totals[1] += failed
                totals[2] += skipped

                if failed:
                    print(f"{name}: {passed} passed, {failed} failed, {skipped} skipped")

                    for f in failures[:5]:
                        print("    " + f)

                if failed > KNOWN.get(name, 0):
                    bad.append(name)

        print(f"TOTAL {totals[0]} passed, {totals[1]} failed, {totals[2]} skipped")

        if bad:
            sys.exit(f"failing beyond the known: {', '.join(bad)}")

    Path(stamp).parent.mkdir(parents=True, exist_ok=True)
    Path(stamp).touch()


if __name__ == "__main__":
    main()
