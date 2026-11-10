#!/usr/bin/env python3
"""Build LLVM's multicall program for this system: clang, lld and the other
tools llvm-driver gathers into one binary, configured as ix builds clang 23
for its hosts (clang, lld and polly, every target, polly linked in, no
shared libraries) and compiled to wasm32 against lib/c and lib/cxx.

    make.py <cc> <c++> <ar> <ranlib> <cmake> <ninja> <llvm-tarball> <llvm-tblgen> <clang-tblgen>
            <libc-include> <libc.a> <cxx-include.tar> <libc++.a> <llvm.wasm> <resource.tar>

The host gives the compilers, cmake, ninja and the release's llvm-tblgen
and clang-tblgen; nothing is built for it. CMake sees Pizdos, a Unix whose
programs are wasm32 modules: dev/wasm.py's flags, lib/cxx's and lib/c's
libraries, and the libraries a program names besides libc empty, as
musl's are, since libc has their functions. A process has one thread, so
LLVM has none. resource.tar is lib/clang/<version>/include, the headers
clang finds beside its program.
"""
import os
import shutil
import subprocess
import sys
import tarfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT.parent.parent / "dev"))

import wasm  # noqa: E402

# libc/ is LLVM libc, whose shared headers LLVM includes; lld takes Mach-O's
# compact unwind encoding from libunwind's headers
PARTS = ["llvm/", "clang/", "lld/", "polly/", "cmake/", "third-party/", "libc/", "libunwind/include/"]
# what only LLVM_INCLUDE_TESTS adds; lld and polly add their docs and polly its
# tests whatever it says
UNUSED = ["llvm/test/", "llvm/unittests/", "clang/test/", "clang/unittests/"]

# The 8 MiB stack ix links clang with, 1 KiB past it: a module keeps nothing
# in its first 1 KiB.
LINK = ["-nostdlib", "-Wl,--stack-first", "-Wl,-z,stack-size=8389632", "-Wl,--max-memory=4294967296",
        "-Wl,--export=__heap_base", "-Wl,--export=__stack_pointer", "-Wl,--gc-sections"]

CONFIG = {
    "CMAKE_BUILD_TYPE": "Release",
    "LLVM_ENABLE_PROJECTS": "clang;lld;polly",
    "LLVM_TARGETS_TO_BUILD": "all",
    "LLVM_TOOL_LLVM_DRIVER_BUILD": "ON",
    "LLVM_BUILD_LLVM_DYLIB": "OFF",
    "LLVM_LINK_LLVM_DYLIB": "OFF",
    "LLVM_POLLY_LINK_INTO_TOOLS": "ON",
    "LLVM_ENABLE_PIC": "OFF",
    "LLVM_ENABLE_PLUGINS": "OFF",
    "LLVM_ENABLE_THREADS": "OFF",
    "LLVM_DEFAULT_TARGET_TRIPLE": "wasm32-unknown-unknown",
    "LLVM_HOST_TRIPLE": "wasm32-unknown-unknown",
    "LLVM_ENABLE_ZLIB": "OFF",
    "LLVM_ENABLE_ZSTD": "OFF",
    "LLVM_ENABLE_LIBXML2": "OFF",
    "LLVM_ENABLE_LIBEDIT": "OFF",
    "LLVM_ENABLE_LIBPFM": "OFF",
    "LLVM_ENABLE_Z3_SOLVER": "OFF",
    "LLVM_ENABLE_BACKTRACES": "OFF",
    "LLVM_ENABLE_CRASH_OVERRIDES": "OFF",
    "LLVM_ENABLE_UNWIND_TABLES": "OFF",
    "LLVM_INCLUDE_TESTS": "OFF",
    "LLVM_INCLUDE_BENCHMARKS": "OFF",
    "LLVM_INCLUDE_EXAMPLES": "OFF",
    "LLVM_INCLUDE_DOCS": "OFF",
    "LLVM_APPEND_VC_REV": "OFF",
    "CLANG_ENABLE_STATIC_ANALYZER": "OFF",
    "CLANG_PLUGIN_SUPPORT": "OFF",
}

PLATFORM = """\
# Pizdos: a Unix whose programs are wasm32 modules linked with lib/c.
include(Platform/UnixPaths)
set(UNIX 1)
set(CMAKE_EXECUTABLE_SUFFIX "")
set(CMAKE_STATIC_LIBRARY_PREFIX "lib")
set(CMAKE_STATIC_LIBRARY_SUFFIX ".a")
"""

# The libraries a program names besides libc.
EMPTY = ["libm.a", "libpthread.a", "librt.a", "libdl.a"]


def extract(tarball, work):
    with tarfile.open(tarball) as archive:
        top = None

        for member in archive:
            name = member.name.split("/", 1)

            if len(name) < 2:
                continue

            top = name[0]

            if any(name[1].startswith(part) for part in PARTS) and not any(name[1].startswith(u) for u in UNUSED):
                archive.extract(member, work, filter="data")

    return work / top


def patch(tree):
    """One case: is_local has none for wasm, and the BSD statfs its default
    reads is not lib/c's statvfs."""
    path = tree / "llvm/lib/Support/Unix/Path.inc"
    anchor = "#elif defined(__EMSCRIPTEN__)\n  // Emscripten doesn't currently support remote filesystem mounts.\n  return true;\n"
    src = path.read_text()

    if src.count(anchor) != 1:
        raise SystemExit("Path.inc's is_local_impl has no Emscripten case")

    case = "#elif defined(__wasm__)\n  // The kernel's files are the host's: whether one is remote it cannot tell.\n  return true;\n"
    path.write_text(src.replace(anchor, anchor + case))


def toolchain(work, cc, cxx, ar, ranlib, libc_include, libc, cxx_include, libcxx):
    # clang's export macros know no wasm object format; every library is static
    flags = [flag for flag in wasm.CFLAGS if not flag.startswith("--target=")] + ["-DCLANG_BUILD_STATIC"]
    cflags = " ".join(flags + ["-isystem", libc_include])
    cxxflags = " ".join(flags + ["-nostdinc++", "-isystem", cxx_include, "-isystem", libc_include])
    ldflags = " ".join(LINK + ["-L" + str(work / "lib")])
    (work / "cmake/Platform").mkdir(parents=True)
    (work / "cmake/Platform/Pizdos.cmake").write_text(PLATFORM)
    path = work / "toolchain.cmake"
    path.write_text(f"""\
list(APPEND CMAKE_MODULE_PATH "{work / "cmake"}")
set(CMAKE_SYSTEM_NAME Pizdos)
set(CMAKE_SYSTEM_PROCESSOR wasm32)
set(CMAKE_C_COMPILER "{cc}")
set(CMAKE_CXX_COMPILER "{cxx}")
set(CMAKE_AR "{ar}")
set(CMAKE_RANLIB "{ranlib}")
set(CMAKE_C_COMPILER_TARGET wasm32-unknown-unknown)
set(CMAKE_CXX_COMPILER_TARGET wasm32-unknown-unknown)
set(CMAKE_C_FLAGS_INIT "{cflags}")
set(CMAKE_CXX_FLAGS_INIT "{cxxflags}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "{ldflags}")
set(CMAKE_C_STANDARD_LIBRARIES "{libc}")
set(CMAKE_CXX_STANDARD_LIBRARIES "{libcxx} {libc}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
""")

    return path


def quiet(command, env):
    """Run command with nothing to say unless it fails: then from the first
    failure on, ninja's, or the end of what it wrote."""
    r = subprocess.run(command, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")

    if r.returncode != 0:
        at = r.stdout.find("FAILED:")
        sys.stderr.write(r.stdout[at:at + 20000] if at >= 0 else r.stdout[-20000:])
        raise SystemExit(f"{Path(command[0]).name} exited {r.returncode}")


def main():
    (cc, cxx, ar, ranlib, cmake, ninja, tarball, llvm_tblgen, clang_tblgen, libc_include, libc, cxx_include_tar, libcxx,
     output, resource_tar) = sys.argv[1:16]

    for tool in (llvm_tblgen, clang_tblgen):
        if not tool:
            raise SystemExit("llvm-tblgen and clang-tblgen of the release are not on PATH")

    work = Path(os.environ.get("TMPDIR") or "/tmp") / "clang"
    shutil.rmtree(work, ignore_errors=True)
    (work / "lib").mkdir(parents=True)
    tree = extract(tarball, work)
    patch(tree)

    with tarfile.open(cxx_include_tar) as archive:
        archive.extractall(work / "cxx", filter="data")

    for name in EMPTY:
        subprocess.run([ar, "rcs", str(work / "lib" / name)], check=True)

    chain = toolchain(work, cc, cxx, ar, ranlib, str(Path(libc_include).resolve()), str(Path(libc).resolve()),
                      str(work / "cxx/include/c++/v1"), str(Path(libcxx).resolve()))
    # the configuration is all here, none from the environment's flags
    env = {k: v for k, v in os.environ.items() if k not in ("CFLAGS", "CXXFLAGS", "CPPFLAGS", "LDFLAGS")}
    build = work / "build"
    quiet([cmake, "-G", "Ninja", "-S", str(tree / "llvm"), "-B", str(build), f"-DCMAKE_MAKE_PROGRAM={ninja}",
           f"-DCMAKE_TOOLCHAIN_FILE={chain}", f"-DLLVM_TABLEGEN={llvm_tblgen}", f"-DCLANG_TABLEGEN={clang_tblgen}",
           *(f"-D{k}={v}" for k, v in CONFIG.items())], env)
    quiet([ninja, "-C", str(build), "--quiet", "-j", str(wasm.jobs()), "llvm-driver", "clang-resource-headers"], env)
    Path(output).parent.mkdir(parents=True, exist_ok=True)
    shutil.copy(build / "bin/llvm", output)
    (version,) = (build / "lib/clang").iterdir()
    wasm.write_tar(resource_tar, build, version / "include")


if __name__ == "__main__":
    main()
