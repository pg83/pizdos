#!/usr/bin/env python3
"""lib/cxx's C++ library under `vm run`: tst/cxx/check.cpp prints one line
of results per area, between its static and thread_local objects' lines,
which must be these. Its root has a /tmp for the files.

    test_cxx.py <vm> <check.o>
"""
import os
import subprocess
import sys
import tempfile

EXPECTED = """\
construct first
construct second
construct thread_local
string world 7 1 -42 123 2.50 xxx 1
containers 1 9 1 b seven 4 0 1 3
streams ff ...42 3.142 true 12 3.5 word 6
files line one 18 3 1 .txt 4
algorithms 15 25 15 1 25
memory 5 1 2 1 2
utilities -1 3 variant 2.5 two 15
locks 1 1 1 2 1
time 1 2500 60
random 1791095845 2469588189546311528 48271
regex 1 2024 15 a#b#
locale C Q 1 0
conversions 0.1 1 255 42 ff   ab 0.33
rtti 1 4 1
destroy thread_local
destroy second
destroy first
"""


def main():
    vm, check = sys.argv[1:3]

    with tempfile.TemporaryDirectory() as root:
        os.mkdir(os.path.join(root, "tmp"))
        r = subprocess.run([vm, "run", "--root", root, "-a", "check", check], capture_output=True, text=True,
                           env={"PATH": "/bin"}, timeout=120)

    if r.returncode != 0 or r.stdout != EXPECTED:
        got, want = r.stdout.splitlines(), EXPECTED.splitlines()

        for i, (g, w) in enumerate(zip(got, want)):
            if g != w:
                print(f"line {i + 1}:\n  got  {g}\n  want {w}")

        print(f"FAIL check: exit {r.returncode}, {len(got)} lines of {len(want)}\n{r.stderr[-2000:]}")
        sys.exit(1)

    print(f"cxx: {len(EXPECTED.splitlines())} lines of results as expected")


if __name__ == "__main__":
    main()
