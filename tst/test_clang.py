#!/usr/bin/env python3
"""clang of this system under `vm run`: LLVM's multicall program, as
/bin/llvm with clang, clang++ and wasm-ld naming it, in a root with lib/c's
and lib/cxx's headers and libraries, compiles and links a C and a C++
program, its driver running cc1 and the linker as processes of their own;
vm compiles what they link and the kernel runs it.

    test_clang.py <vm> <llvm.o> <llvm.wasm> <resource.tar> <libc.a> <libc-include.tar> <libc++.a> <cxx-include.tar>
"""
import shutil
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "dev"))

import wasm  # noqa: E402

C = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int compare(const void* a, const void* b) {
    return *(const int*)a - *(const int*)b;
}

int main(int argc, char** argv) {
    int v[] = {5, 3, 9, 1, 7};
    qsort(v, 5, sizeof v[0], compare);
    char buf[64];
    snprintf(buf, sizeof buf, "%d %d %d %d %d %.3f", v[0], v[1], v[2], v[3], v[4], 22.0 / 7);
    printf("%s: %s %zu\n", argv[0], buf, strlen(buf));
    return 0;
}
"""

CXX = r"""
#include <algorithm>
#include <format>
#include <iostream>
#include <map>
#include <string>
#include <vector>

int main() {
    std::vector<std::string> words{"pear", "apple", "fig"};
    std::sort(words.begin(), words.end());
    std::map<std::string, size_t> lengths;
    for (const auto& w : words) {
        lengths[w] = w.size();
    }
    std::cout << std::format("{} {} {}", words[0], lengths["pear"], words.size()) << std::endl;
    return 0;
}
"""

LINK = ["-nostdlib", "-Wl,--stack-first", "-Wl,-z,stack-size=1049600", "-Wl,--max-memory=4294967296",
        "-Wl,--export=__heap_base", "-Wl,--export=__stack_pointer", "-Wl,--gc-sections"]


def run(vm, root, name, program, *args):
    r = subprocess.run([vm, "run", "--root", str(root), "-a", name, "--", str(program), *args], capture_output=True,
                       text=True, env={"PATH": "/bin"}, timeout=600)

    if r.returncode != 0:
        print(f"FAIL {name} {' '.join(args)}: exit {r.returncode}\n{r.stdout[-2000:]}{r.stderr[-2000:]}")
        sys.exit(1)

    return r.stdout


def main():
    vm, llvm, _, resource, libc, libc_include, libcxx, cxx_include = sys.argv[1:9]

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)

        for d in ("bin", "usr/lib", "src", "tmp"):
            (root / d).mkdir(parents=True)

        shutil.copy(llvm, root / "bin" / "llvm")
        (root / "bin" / "llvm").chmod(0o755)

        for name in ("clang", "clang++", "wasm-ld"):
            (root / "bin" / name).symlink_to("llvm")

        for archive, at in ((resource, root), (libc_include, root / "usr"), (cxx_include, root / "usr")):
            with tarfile.open(archive) as t:
                t.extractall(at, filter="data")

        shutil.copy(libc, root / "usr/lib/libc.a")
        shutil.copy(libcxx, root / "usr/lib/libc++.a")
        (root / "src/hello.c").write_text(C)
        (root / "src/hello.cpp").write_text(CXX)
        clang = root / "bin" / "llvm"
        version = run(vm, root, "clang", clang, "--version").splitlines()[0]

        if not version.startswith("clang version 23."):
            print(f"FAIL clang --version: {version}")
            sys.exit(1)

        run(vm, root, "clang", clang, *wasm.CFLAGS, "-isystem", "/usr/include", "-O2", "/src/hello.c",
            "-o", "/tmp/hello.wasm", *LINK, "/usr/lib/libc.a")
        run(vm, root, "clang++", clang, *wasm.CFLAGS, "-nostdinc++", "-fno-exceptions", "-std=c++23",
            "-isystem", "/usr/include/c++/v1", "-isystem", "/usr/include", "-O2", "/src/hello.cpp",
            "-o", "/tmp/hello-cxx.wasm", *LINK, "/usr/lib/libc++.a", "/usr/lib/libc.a")
        results = []

        for name, want in (("hello", "hello: 1 3 5 7 9 3.143 15\n"), ("hello-cxx", "apple 4 3\n")):
            program = root / "bin" / name
            subprocess.run([vm, "-o", str(program), str(root / "tmp" / f"{name}.wasm")], check=True)
            program.chmod(0o755)
            got = run(vm, root, name, program)

            if got != want:
                print(f"FAIL {name}: got {got!r}, want {want!r}")
                sys.exit(1)

            results.append(name)

    print(f"clang: {version}; built {' and '.join(results)}, which ran as expected")


if __name__ == "__main__":
    main()
