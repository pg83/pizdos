#!/usr/bin/env python3
"""clang of this system under `vm run`: LLVM's multicall program, as
/bin/llvm with clang, clang++ and wasm-ld naming it, in a root with lib/c's
and lib/cxx's headers and libraries and busybox's shell, compiles and links
a C and a C++ program, its driver running cc1 and the linker as processes of
their own, and the shell runs what they link, packed by pzd into
executables: the kernel compiles each module once, LLVM's for all its
processes.

    test_clang.py <vm> <busybox> <busybox.wasm> <busybox.links> <llvm> <llvm.wasm> <resource.tar> <libc.a> <libc-include.tar> <libc++.a> <cxx-include.tar> <pzd>
"""
import shlex
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


def main():
    vm, busybox, _, links, llvm, _, resource, libc, libc_include, libcxx, cxx_include, pzd = sys.argv[1:13]

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)

        for d in ("bin", "usr/lib", "src", "tmp"):
            (root / d).mkdir(parents=True)

        shutil.copy(llvm, root / "bin" / "llvm")
        (root / "bin" / "llvm").chmod(0o755)
        shutil.copy(busybox, root / "bin" / "busybox")
        (root / "bin" / "busybox").chmod(0o755)
        shutil.copy(pzd, root / "bin" / "pzd")
        (root / "bin" / "pzd").chmod(0o755)

        for name in ("clang", "clang++", "wasm-ld"):
            (root / "bin" / name).symlink_to("llvm")

        for link in Path(links).read_text().split():
            path = root / "bin" / Path(link).name

            if not path.exists() and not path.is_symlink():
                path.symlink_to("busybox")

        for archive, at in ((resource, root), (libc_include, root / "usr"), (cxx_include, root / "usr")):
            with tarfile.open(archive) as t:
                t.extractall(at, filter="data")

        shutil.copy(libc, root / "usr/lib/libc.a")
        shutil.copy(libcxx, root / "usr/lib/libc++.a")
        (root / "src/hello.c").write_text(C)
        (root / "src/hello.cpp").write_text(CXX)
        c = ["clang", *wasm.CFLAGS, "-isystem", "/usr/include", "-O2", "/src/hello.c", "-o", "/src/hello.wasm", *LINK, "/usr/lib/libc.a"]
        cxx = ["clang++", *wasm.CFLAGS, "-nostdinc++", "-fno-exceptions", "-std=c++23", "-isystem", "/usr/include/c++/v1",
               "-isystem", "/usr/include", "-O2", "/src/hello.cpp", "-o", "/src/hello-cxx.wasm", *LINK, "/usr/lib/libc++.a", "/usr/lib/libc.a"]
        script = (f"set -e\nclang --version\necho ---\n{shlex.join(c)}\n{shlex.join(cxx)}\n"
                  "pzd /src/hello.wasm /bin/hello\npzd /src/hello-cxx.wasm /bin/hello-cxx\nhello\nhello-cxx\n")
        r = subprocess.run([vm, "run", "--root", str(root), "-a", "sh", "--", str(root / "bin" / "busybox"), "-c", script],
                           capture_output=True, text=True, env={"PATH": "/bin"}, timeout=1200)
        version, _, ran = r.stdout.partition("---\n")
        version = version.splitlines()[0] if version else ""
        want = "hello: 1 3 5 7 9 3.143 15\napple 4 3\n"

        if r.returncode != 0 or not version.startswith("clang version 23.") or ran != want:
            print(f"FAIL clang: exit {r.returncode}, version {version!r}, ran {ran!r}, want {want!r}\n{r.stderr[-3000:]}")
            sys.exit(1)

    print(f"clang: {version}; built hello and hello-cxx, which ran as expected")


if __name__ == "__main__":
    main()
