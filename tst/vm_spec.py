#!/usr/bin/env python3
"""The WebAssembly spec testsuite through `vm spec`.

    vm_spec.py <vm> <wast2json> <testsuite.tar.gz> <stamp> [native-aot]

Every top-level .wast is converted by wast2json and run by vm, which
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

# Failures of what vm does not implement, by file: table.grow, which
# fails; modules of two memories, skipped, which were to fill memory or a
# table that other modules read.
KNOWN = {
    "linking0.wast": 1,
    "load1.wast": 5,
    "table_grow.wast": 30,
    "table_size.wast": 15,
}


def run(vm, wast2json, wast, work, native=None):
    work.mkdir(parents=True)
    r = subprocess.run([wast2json, str(wast), "-o", str(work / "t.json")], capture_output=True, text=True)

    if r.returncode != 0:
        return None

    compiled = 0
    if native:
        environment = dict(os.environ, W2X_MEMORY_TAIL="strict", W2X_TAIL_PROFILE="")
        for module in sorted(work.glob("*.wasm")):
            data = module.read_bytes()
            host = subprocess.run([native, "--strict"], input=data, capture_output=True, timeout=60)
            target = work / "comparison.o"
            reactor = subprocess.run([vm, "-o", str(target), str(module)], capture_output=True, env=environment, timeout=60)
            if bool(host.returncode) != bool(reactor.returncode):
                return f"compiler acceptance differs for {module.name}: native {host.returncode}, Wasm {reactor.returncode}"
            if host.returncode == 0:
                if host.stdout != target.read_bytes():
                    return f"native/Wasm objects differ for {module.name}"
            elif not host.stderr.strip() or host.stderr.strip() not in reactor.stderr:
                return f"compiler rejection differs for {module.name}: native {host.stderr[-500:]!r}, Wasm {reactor.stderr[-500:]!r}"
            compiled += 1

    try:
        r = subprocess.run([vm, "spec", str(work / "t.json")], capture_output=True, text=True, timeout=600)
    except subprocess.TimeoutExpired:
        return "timeout"

    m = re.search(r"^RESULT (\d+) (\d+) (\d+)$", r.stdout, re.M)

    if r.returncode != 0 or m is None:
        return f"crashed ({r.returncode}): " + "\n".join((r.stdout + r.stderr).strip().splitlines()[-5:])

    failures = [line for line in r.stdout.splitlines() if line.startswith("FAIL")]

    return int(m.group(1)), int(m.group(2)), int(m.group(3)), failures, compiled


def main():
    vm, wast2json, tarball, stamp, *compiler = sys.argv[1:]
    native = compiler[0] if compiler else None

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
        compiled = 0

        with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count()) as pool:
            jobs = {w.name: pool.submit(run, vm, wast2json, w, tmp / "work" / w.stem, native) for w in wasts}

            for name, job in jobs.items():
                res = job.result()

                if res is None:
                    continue

                if isinstance(res, str):
                    print(f"{name}: {res}")
                    bad.append(name)

                    continue

                passed, failed, skipped, failures, modules = res
                compiled += modules
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
        if native:
            print(f"native/Wasm AOT: {compiled} spec modules have identical objects or matching rejections")

        if bad:
            sys.exit(f"failing beyond the known: {', '.join(bad)}")

    Path(stamp).parent.mkdir(parents=True, exist_ok=True)
    Path(stamp).touch()


if __name__ == "__main__":
    main()
