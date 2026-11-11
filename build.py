import build
import json
import os
import shutil


def tool(name):
    path = shutil.which(name)

    if path is None:
        raise RuntimeError(f"{name} is not on PATH")

    return path


def touch(path):
    return [
        "python3",
        "-c",
        f"from pathlib import Path; p=Path(r'{path}'); p.parent.mkdir(parents=True, exist_ok=True); p.touch()",
    ]


# Tool paths are absolute in every command line, so a toolchain change is a
# node UID change.
CLANG = tool("clang")
CLANGXX = tool("clang++")
LLVM_AR = tool("llvm-ar")
LLVM_RANLIB = tool("llvm-ranlib")
WASM_LD = tool("wasm-ld")
LD_LLD = tool("ld.lld")
GO = tool("go")

MUSL = "1.2.5"
BUSYBOX = "1.36.1"
DOOMGENERIC = "e257a09527062bfb986b6e5e0780839b6cbd5b71"
PYTHON = "3.14.10"
STR = "1318a0f603c8d23839f02840cc1d9129c82f0839"
LLVM = "23.1.5"


def fetch(name, url, sha256, output):
    return command(
        name=name,
        local=True,
        inputs=["$(S)/dev/fetch.py"],
        outputs=[output],
        cmd=["python3", "$(S)/dev/fetch.py", url, sha256, output],
        descr="FE",
        color="blue",
    )


musl_tarball = fetch(
    "musl_tarball",
    f"https://musl.libc.org/releases/musl-{MUSL}.tar.gz",
    "a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4",
    f"$(B)/ext/musl-{MUSL}.tar.gz",
)

busybox_tarball = fetch(
    "busybox_tarball",
    f"https://busybox.net/downloads/busybox-{BUSYBOX}.tar.bz2",
    "b8cc24c9574d809e7279c3be349795c5d5ceb6fdf19ca709f80cde50e47de314",
    f"$(B)/ext/busybox-{BUSYBOX}.tar.bz2",
)

doomgeneric_tarball = fetch(
    "doomgeneric_tarball",
    f"https://github.com/ozkl/doomgeneric/archive/{DOOMGENERIC}.tar.gz",
    "26f8f60daa6c6e4d3c01e5eac9fb700d592dede803d13ac848154ff356b2a2e7",
    f"$(B)/ext/doomgeneric-{DOOMGENERIC}.tar.gz",
)

python_tarball = fetch(
    "python_tarball",
    f"https://www.python.org/ftp/python/{PYTHON}/Python-{PYTHON}.tgz",
    "9e4996c19ae5a3df31f10c37723661f71d3455d73bd005f79df7098e5cc13192",
    f"$(B)/ext/Python-{PYTHON}.tgz",
)

str_tarball = fetch(
    "str_tarball",
    f"https://github.com/pg83/str/archive/{STR}.tar.gz",
    "750a476204f93a453825afe23143e44c6f54bfda944cbd55fd22b6c0f1c7ee6b",
    f"$(B)/ext/str-{STR}.tar.gz",
)

llvm_tarball = fetch(
    "llvm_tarball",
    f"https://github.com/llvm/llvm-project/releases/download/llvmorg-{LLVM}/llvm-project-{LLVM}.src.tar.xz",
    "fb50b1eac93ddeb3aa8dab4cfe324dff74f4a9c2bdee6160c9e36188f8d06a6e",
    f"$(B)/ext/llvm-project-{LLVM}.src.tar.xz",
)

# doom1.wad of the shareware 1.9, the last one id released
doom_wad = fetch(
    "doom_wad",
    "https://silicon-pirates.com/doom/doom1.wad",
    "1d7d43be501e67d927e415e0b8f3e29c3bf33075e859721816f652a526cac771",
    "$(B)/ext/doom/doom1.wad",
)

LIBC_HEADERS = build.glob("$(S)/lib/c/include/**/*.h")

# lib/c: the in-process part of the C library, our sources plus the musl
# subset named in lib/c/musl/files.txt; every other libc function is a kernel
# import declared by its header.
libc = command(
    name="libc",
    inputs=[
        *LIBC_HEADERS,
        *build.glob("$(S)/lib/c/musl/*"),
        *build.glob("$(S)/lib/c/src/*"),
        "$(S)/lib/c/make.py",
        "$(S)/dev/wasm.py",
    ],
    outputs=["$(B)/lib/c/libc.a"],
    deps=[musl_tarball],
    cmd=["python3", "$(S)/lib/c/make.py", CLANG, LLVM_AR, musl_tarball.outputs[0], "$(B)/lib/c/libc.a"],
    descr="CC",
    color="green",
)

# lib/c's headers as a program compiled inside the system finds them.
libc_headers = command(
    name="libc_headers",
    inputs=[*LIBC_HEADERS, "$(S)/lib/c/make.py", "$(S)/dev/wasm.py"],
    outputs=["$(B)/lib/c/include.tar"],
    cmd=["python3", "$(S)/lib/c/make.py", "headers", "$(B)/lib/c/include.tar"],
    descr="HD",
    color="green",
)

# lib/cxx: libc++ and libc++abi of the LLVM release, for lib/c: the library
# and include/c++/v1, the headers a C++ program includes before lib/c's.
libcxx = command(
    name="libcxx",
    inputs=[*LIBC_HEADERS, "$(S)/lib/cxx/make.py", "$(S)/lib/cxx/__config_site", "$(S)/dev/wasm.py"],
    outputs=["$(B)/lib/cxx/libc++.a", "$(B)/lib/cxx/include.tar"],
    deps=[llvm_tarball],
    cmd=["python3", "$(S)/lib/cxx/make.py", CLANG, LLVM_AR, llvm_tarball.outputs[0], "$(S)/lib/c/include",
         "$(B)/lib/cxx/libc++.a", "$(B)/lib/cxx/include.tar"],
    descr="CC",
    color="green",
)

# lib/str: pg83/str's string and memory functions, compiled by str's own
# compiler in the convention of the code wasm2o compiles, for its loader.
libstr = command(
    name="libstr",
    inputs=["$(S)/lib/str/make.py"],
    outputs=["$(B)/lib/str/libstr.o"],
    deps=[str_tarball],
    cmd=["python3", "$(S)/lib/str/make.py", str_tarball.outputs[0], "$(B)/lib/str/libstr.o"],
    descr="CC",
    color="green",
)

busybox = command(
    name="busybox",
    inputs=[
        *LIBC_HEADERS,
        "$(S)/bin/busybox/config",
        "$(S)/bin/busybox/gen.py",
        "$(S)/bin/busybox/make.py",
        "$(S)/dev/wasm.py",
    ],
    outputs=["$(B)/bin/busybox/busybox.wasm", "$(B)/bin/busybox/busybox.links"],
    deps=[busybox_tarball, libc],
    cmd=[
        "python3", "$(S)/bin/busybox/make.py",
        CLANG, LLVM_AR, WASM_LD,
        busybox_tarball.outputs[0],
        "$(S)/bin/busybox/config",
        "$(S)/lib/c/include",
        libc.outputs[0],
        "$(B)/bin/busybox/busybox.wasm",
        "$(B)/bin/busybox/busybox.links",
    ],
    descr="BB",
    color="cyan",
)

# The kernel ABI busybox needs: every import and export of the module.
busybox_abi = command(
    name="busybox_abi",
    inputs=["$(S)/dev/wasm_info.py"],
    outputs=["$(B)/bin/busybox/abi.txt"],
    deps=[busybox],
    cmd=["python3", "$(S)/dev/wasm_info.py", busybox.outputs[0], "$(B)/bin/busybox/abi.txt"],
    descr="AB",
    color="magenta",
)

# doom: doomgeneric with our terminal backend, drawn as ANSI half blocks on
# the tty the kernel passes through.
doom = command(
    name="doom",
    inputs=[
        *LIBC_HEADERS,
        "$(S)/bin/doom/doomgeneric_pizdos.c",
        "$(S)/bin/doom/make.py",
        "$(S)/dev/wasm.py",
    ],
    outputs=["$(B)/bin/doom/doom.wasm"],
    deps=[doomgeneric_tarball, libc],
    cmd=[
        "python3", "$(S)/bin/doom/make.py",
        CLANG, WASM_LD,
        doomgeneric_tarball.outputs[0],
        "$(S)/bin/doom/doomgeneric_pizdos.c",
        "$(S)/lib/c/include",
        libc.outputs[0],
        "$(B)/bin/doom/doom.wasm",
    ],
    descr="DM",
    color="light-magenta",
)

# CPython's interpreter and built-in modules, and its freezer: nothing of
# wasm2o, so a change of the compiler rebuilds none of it.
python_core = command(
    name="python_core",
    inputs=[*LIBC_HEADERS, "$(S)/bin/python/pyconfig.h", "$(S)/bin/python/make.py", "$(S)/dev/wasm.py"],
    outputs=["$(B)/bin/python/libpython.a", "$(B)/bin/python/freeze.wasm"],
    deps=[python_tarball, libc],
    cmd=[
        "python3", "$(S)/bin/python/make.py", "core",
        CLANG, LLVM_AR, WASM_LD,
        python_tarball.outputs[0],
        "$(S)/bin/python/pyconfig.h",
        "$(S)/lib/c/include",
        libc.outputs[0],
        "$(B)/bin/python/libpython.a",
        "$(B)/bin/python/freeze.wasm",
    ],
    descr="PY",
    color="yellow",
)

# bin/aot is a single freestanding C++ translation unit. The WASI reactor
# exports compile(), reading fd 0 and writing fd 1; the command adds _start.
AOT_FLAGS = ["-x", "c++", "-std=c++20", "-O2", "-fno-exceptions", "-fno-rtti", "-fno-builtin", "-fwrapv", "-ffp-contract=off", "-Wno-parentheses-equality"]
# Compiler debug names otherwise get duplicated in ELF symbols and Go's
# runtime metadata when wasm2o embeds the compiled reactor. The multivalue
# ABI passes and returns small structs, the compiler's slices and views
# among them, as values instead of through the shadow stack.
AOT_WASM_FLAGS = [*AOT_FLAGS, "--target=wasm32-unknown-unknown", "-ffreestanding", "-nostdlib", "-nostdinc", "-nostdinc++", "-mbulk-memory", "-Xclang", "-target-abi", "-Xclang", "experimental-mv", "-Wl,--strip-all", "-Wl,--no-entry", "-Wl,--export-memory", "-Wl,--stack-first", "-Wl,-z,stack-size=4195328"]

aot = command(
    name="aot",
    inputs=["$(S)/bin/aot/aot.cpp"],
    outputs=["$(B)/bin/aot/aot.wasm"],
    cmd=[CLANG, *AOT_WASM_FLAGS, "$(S)/bin/aot/aot.cpp", "-o", "$(B)/bin/aot/aot.wasm"],
    descr="CC", color="green",
)

aot_command = command(
    name="aot_command",
    inputs=["$(S)/bin/aot/aot.cpp"],
    outputs=["$(B)/bin/aot/aot-command.wasm"],
    cmd=[CLANG, *AOT_WASM_FLAGS, "-DAOT_COMMAND", "$(S)/bin/aot/aot.cpp", "-o", "$(B)/bin/aot/aot-command.wasm"],
    descr="CC", color="green",
)

aot_selftest = command(
    name="aot_selftest",
    inputs=["$(S)/bin/aot/aot.cpp"],
    outputs=["$(B)/bin/aot/aot-selftest.wasm"],
    cmd=[CLANG, *AOT_WASM_FLAGS, "-DAOT_SELF_TEST", "$(S)/bin/aot/aot.cpp", "-o", "$(B)/bin/aot/aot-selftest.wasm"],
    descr="CC", color="green",
)

# A native build needs host libc headers/libraries, supplied by the usual
# CPPFLAGS/LDFLAGS. The C driver links libc without a C++ runtime.
aot_native = command(
    name="aot_native",
    inputs=["$(S)/bin/aot/aot.cpp"],
    outputs=["$(B)/bin/aot/aot"],
    cmd=[CLANG, *build.cppflags, *build.cxxflags, *AOT_FLAGS, "-O3", "$(S)/bin/aot/aot.cpp", "-o", "$(B)/bin/aot/aot", *build.ldflags],
    descr="CC", color="green",
)

aot_measure = command(
    name="aot_measure",
    inputs=["$(S)/dev/aot_measure.c"],
    outputs=["$(B)/dev/aot_measure"],
    cmd=[CLANG, *build.cppflags, "-O2", "$(S)/dev/aot_measure.c", "-o", "$(B)/dev/aot_measure", *build.ldflags],
    descr="CC", color="green",
)

# Bootstrap without an existing wasm2o: the native C++ compiler compiles the
# WASI reactor to the machine-code object embedded in the Go loader.
aot_aot = command(
    name="aot_aot",
    inputs=["$(S)/dev/aot.py"],
    outputs=["$(B)/bin/aot/aot.aot"],
    deps=[aot_native, aot],
    cmd=["python3", "$(S)/dev/aot.py", aot_native.outputs[0], aot.outputs[0], "$(B)/bin/aot/aot.aot"],
    descr="AO", color="light-cyan",
)

aot_command_aot = command(
    name="aot_command_aot",
    inputs=["$(S)/dev/aot.py"],
    outputs=["$(B)/bin/aot/aot-command.aot"],
    deps=[aot_native, aot_command],
    cmd=["python3", "$(S)/dev/aot.py", aot_native.outputs[0], aot_command.outputs[0], "$(B)/bin/aot/aot-command.aot"],
    descr="AO", color="light-cyan",
)

WASM2O_SOURCES = [*build.glob("$(S)/bin/vm/*.go"), *build.glob("$(S)/bin/vm/*.s"), "$(S)/bin/vm/go.mod", "$(S)/dev/go.py"]
GO_RUNTIME = [*build.glob("$(S)/lib/goruntime/*.go"), "$(S)/lib/goruntime/edits.py"]

wasm2o = command(
    name="wasm2o",
    inputs=[*WASM2O_SOURCES, *GO_RUNTIME],
    outputs=["$(B)/bin/vm/wasm2o"],
    deps=[aot_aot, libstr],
    cmd=["python3", "$(S)/dev/go.py", "build", GO, "$(S)/bin/vm", "$(B)/bin/vm/wasm2o", "--runtime", "$(S)/lib/goruntime", "--embed", aot_aot.outputs[0],
         "--embed", libstr.outputs[0]],
    descr="GO", color="light-blue",
)

# busybox.wasm compiled by wasm2o: what `wasm2o run` runs in the kernel's
# processes.
busybox_o = command(
    name="busybox_o",
    outputs=["$(B)/bin/busybox/busybox.o"],
    deps=[wasm2o, busybox],
    cmd=[wasm2o.outputs[0], "-o", "$(B)/bin/busybox/busybox.o", busybox.outputs[0]],
    descr="AO",
    color="light-cyan",
)

doom_o = command(
    name="doom_o",
    outputs=["$(B)/bin/doom/doom.o"],
    deps=[wasm2o, doom],
    cmd=[wasm2o.outputs[0], "-o", "$(B)/bin/doom/doom.o", doom.outputs[0]],
    descr="AO",
    color="light-cyan",
)

# python.wasm, its modules frozen by the freezer running in the kernel, and
# the standard library it imports from a zip.
python = command(
    name="python",
    inputs=[*LIBC_HEADERS, "$(S)/bin/python/pyconfig.h", "$(S)/bin/python/make.py", "$(S)/dev/wasm.py"],
    outputs=["$(B)/bin/python/python.wasm", "$(B)/bin/python/python314.zip"],
    deps=[python_tarball, libc, python_core, wasm2o],
    cmd=[
        "python3", "$(S)/bin/python/make.py", "python",
        CLANG, WASM_LD, wasm2o.outputs[0],
        python_tarball.outputs[0],
        "$(S)/bin/python/pyconfig.h",
        "$(S)/lib/c/include",
        libc.outputs[0],
        *python_core.outputs,
        "$(B)/bin/python/python.wasm",
        "$(B)/bin/python/python314.zip",
    ],
    descr="PY",
    color="yellow",
)

python_o = command(
    name="python_o",
    outputs=["$(B)/bin/python/python.o"],
    deps=[wasm2o, python],
    cmd=[wasm2o.outputs[0], "-o", "$(B)/bin/python/python.o", python.outputs[0]],
    descr="AO",
    color="light-cyan",
)

# clang for this system: LLVM's multicall program of the release lib/cxx
# comes from, compiled against lib/c and lib/cxx, and the headers clang
# keeps beside it. The host gives cmake, ninja and the release's
# llvm-tblgen and clang-tblgen on PATH.
LLVM_TBLGEN = shutil.which("llvm-tblgen") or ""
CLANG_TBLGEN = shutil.which("clang-tblgen") or ""

clang = command(
    name="clang",
    inputs=[*LIBC_HEADERS, "$(S)/bin/clang/make.py", "$(S)/dev/wasm.py"],
    outputs=["$(B)/bin/clang/llvm.wasm", "$(B)/bin/clang/resource.tar"],
    deps=[llvm_tarball, libc, libcxx],
    cmd=[
        "python3", "$(S)/bin/clang/make.py",
        CLANG, CLANGXX, LLVM_AR, LLVM_RANLIB,
        shutil.which("cmake") or "cmake", shutil.which("ninja") or "ninja",
        llvm_tarball.outputs[0], LLVM_TBLGEN, CLANG_TBLGEN,
        "$(S)/lib/c/include", libc.outputs[0], libcxx.outputs[1], libcxx.outputs[0],
        "$(B)/bin/clang/llvm.wasm", "$(B)/bin/clang/resource.tar",
    ],
    descr="CL",
    color="green",
)

clang_o = command(
    name="clang_o",
    outputs=["$(B)/bin/clang/llvm.o"],
    deps=[wasm2o, clang],
    cmd=[wasm2o.outputs[0], "-o", "$(B)/bin/clang/llvm.o", clang.outputs[0]],
    descr="AO",
    color="light-cyan",
)

# test_libc's program: lib/c's functions with deterministic results.
libc_check = command(
    name="libc_check",
    inputs=[*LIBC_HEADERS, "$(S)/tst/libc/check.c", "$(S)/tst/libc/make.py", "$(S)/dev/wasm.py"],
    outputs=["$(B)/tst/libc/check.wasm"],
    deps=[libc],
    cmd=["python3", "$(S)/tst/libc/make.py", CLANG, WASM_LD, "$(S)/tst/libc/check.c", "$(S)/lib/c/include", libc.outputs[0], "$(B)/tst/libc/check.wasm"],
    descr="CC",
    color="green",
)

libc_check_o = command(
    name="libc_check_o",
    outputs=["$(B)/tst/libc/check.o"],
    deps=[wasm2o, libc_check],
    cmd=[wasm2o.outputs[0], "-o", "$(B)/tst/libc/check.o", libc_check.outputs[0]],
    descr="AO",
    color="light-cyan",
)

# test_cxx's program: lib/cxx's library over lib/c with deterministic results.
cxx_check = command(
    name="cxx_check",
    inputs=[*LIBC_HEADERS, "$(S)/tst/cxx/check.cpp", "$(S)/tst/cxx/make.py", "$(S)/dev/wasm.py"],
    outputs=["$(B)/tst/cxx/check.wasm"],
    deps=[libc, libcxx],
    cmd=["python3", "$(S)/tst/cxx/make.py", CLANG, WASM_LD, "$(S)/tst/cxx/check.cpp", "$(S)/lib/c/include", libc.outputs[0],
         libcxx.outputs[1], libcxx.outputs[0], "$(B)/tst/cxx/check.wasm"],
    descr="CC",
    color="green",
)

cxx_check_o = command(
    name="cxx_check_o",
    outputs=["$(B)/tst/cxx/check.o"],
    deps=[wasm2o, cxx_check],
    cmd=[wasm2o.outputs[0], "-o", "$(B)/tst/cxx/check.o", cxx_check.outputs[0]],
    descr="AO",
    color="light-cyan",
)

# Exercise an ordinary linker's relocations on the self-compiled command.
aot_relinked = command(
    name="aot_relinked",
    outputs=["$(B)/bin/aot/aot-command.relinked.aot"],
    deps=[aot_command_aot],
    cmd=[LD_LLD, "-r", aot_command_aot.outputs[0], "-o", "$(B)/bin/aot/aot-command.relinked.aot"],
    descr="LD", color="green",
)

# wasm2o's own tests: the loader drives Go's runtime through native frames.
wasm2o_go_test = command(
    name="test_wasm2o_go",
    inputs=[*build.glob("$(S)/bin/vm/*.go"), *build.glob("$(S)/bin/vm/*.s"), "$(S)/bin/vm/go.mod", "$(S)/dev/go.py", *GO_RUNTIME],
    outputs=["$(B)/tst/wasm2o_go.stamp"],
    deps=[aot_aot, libstr],
    cmd=["python3", "$(S)/dev/go.py", "test", GO, "$(S)/bin/vm", "$(B)/tst/wasm2o_go.stamp", "--runtime", "$(S)/lib/goruntime", "--embed", aot_aot.outputs[0],
         "--embed", libstr.outputs[0]],
    descr="TS",
    color="green",
)

# The WebAssembly spec testsuite through `wasm2o spec`; wast2json comes from
# wabt, under ix run bld/wabt.
TESTSUITE = "9b9fbf8260507169066bf3e8de7b01fb8c145e55"
WAST2JSON = shutil.which("wast2json") or ""

testsuite_tarball = fetch(
    "testsuite_tarball",
    f"https://github.com/WebAssembly/testsuite/archive/{TESTSUITE}.tar.gz",
    "0d0ed50f55f17f25b5db9e28d3d84d4a0a9feb5e97fd9a718f6d7017b16eb2e4",
    f"$(B)/ext/testsuite-{TESTSUITE}.tar.gz",
)

wasm2o_spec_test = command(
    name="test_wasm2o_spec",
    inputs=["$(S)/tst/wasm2o_spec.py"],
    outputs=["$(B)/tst/wasm2o_spec.stamp"],
    deps=[wasm2o, testsuite_tarball],
    cmd=["python3", "$(S)/tst/wasm2o_spec.py", wasm2o.outputs[0], WAST2JSON, testsuite_tarball.outputs[0], "$(B)/tst/wasm2o_spec.stamp"],
    descr="TS",
    color="green",
)

# tst/wazero: a WASI runner on wazero, what wasm2o is measured against. Its
# modules, checked against go.sum, are fetched once; the build has no network.
wazero_modules = command(
    name="wazero_modules",
    local=True,
    inputs=["$(S)/dev/go.py", "$(S)/tst/wazero/go.mod", "$(S)/tst/wazero/go.sum"],
    outputs=["$(B)/tst/wazero/modules.tar"],
    cmd=["python3", "$(S)/dev/go.py", "download", GO, "$(S)/tst/wazero", "$(B)/tst/wazero/modules.tar"],
    descr="FE",
    color="blue",
)

wazero = command(
    name="wazero",
    inputs=[*build.glob("$(S)/tst/wazero/*.go"), "$(S)/tst/wazero/go.mod", "$(S)/tst/wazero/go.sum", "$(S)/dev/go.py"],
    outputs=["$(B)/tst/wazero/wazero"],
    deps=[wazero_modules],
    cmd=[
        "python3", "$(S)/dev/go.py", "build", GO, "$(S)/tst/wazero", "$(B)/tst/wazero/wazero",
        "--modules", wazero_modules.outputs[0],
    ],
    descr="GO",
    color="light-blue",
)

# The stand compares wasm2o's code with native code of the same programs:
# the C++ compiler, and decode release 16, pure C/C++ wasm whose native
# column uses the same wrappers and the libraries of the ix revision that
# built that release. dev/stand/build.sh supplies those libraries and the
# host toolchain.
DECODE_IX = "53f12e265c480e7849bfa58eedf608db63020b1b"
DECODE = {
    "im": ("im", "0a168cb80eb61c6540a4e445a509f0d57b086565e99da0605671fb8d61b99783", "lib/image/magick/wasm/decode.c", "06a48395995911dc4f6ddeaf5fd07b13232de1872eb5d5f340fc01cad735a8c1"),
    "pdf": ("pdf", "ab2c0067403b605b2bdc2009e4bb0a48df80511a19c5ccca9785f27dd9d4df0d", "lib/pdf/ium/wasm/pdf.c", "e6ae5484e10deb16e338a4dbc7dc9d9f0d8834b51f9bff397eba6b4eb6282e65"),
    "djvu": ("djvu", "82468c78dcc7d9a3a5f2cd2d9a7cc961b215e485f09104cdefa3da11e32137a3", "lib/djvulibre/wasm/djvu.cpp", "68cecb4c5947fdb340e56a9d8627f7a16ce1f286279e43826aeff8f253ae5e5c"),
    "svg": ("svg_simple", "377cdcf753d46b07c749a2a593edbd7bc3c6539537779591f6ba9af93ee48ebb", "lib/svg/wasm/simple/svg.c", "054cb55d74726ef04be7c03af517b016d5207ab2854f0f6b5dffbec785af33b8"),
    "magic": ("magic", "3d5a2a4a148126b587836004d1e09e0c90d2c062b6b94179c4f40c57b092d41e", "lib/magic/wasm/magic.c", "9caceafefa48fed7ef7ca358f249c0b4347f595f79b5976d133ca90738e9a658"),
}
NATIVE_CONFIG = json.dumps({
    "cc": CLANG,
    "ar": LLVM_AR,
    "cxx": shutil.which("clang++") or "clang++",
    "pkg_config": shutil.which("pkg-config") or "pkg-config",
    "cppflags": build.cppflags,
    "cflags": build.cflags,
    "cxxflags": build.cxxflags,
    "ldflags": build.ldflags,
    "magic_data": os.environ.get("MAGIC_DATA", ""),
    "ix": os.environ.get("STAND_IX_REV", ""),
    "scalar": os.environ.get("STAND_NATIVE_SCALAR", ""),
}, sort_keys=True)
decode_stand = []

for kind, (module, digest, source, source_digest) in DECODE.items():
    folder = f"$(B)/stand/decode/{kind}"
    packed = fetch(f"stand_{kind}_download", f"https://github.com/impulse-desktop/decode/releases/download/16/{module}.wasm.zstd", digest, f"{folder}/input.wasm.zstd")
    wasm = command(
        name=f"stand_{kind}_wasm",
        deps=[packed],
        outputs=[f"{folder}/input.wasm"],
        cmd=[shutil.which("zstd") or "zstd", "-d", "-q", "-f", packed.outputs[0], "-o", f"{folder}/input.wasm"],
        descr="UZ", color="blue",
    )
    obj = command(
        name=f"stand_{kind}_aot",
        deps=[wasm2o, wasm],
        outputs=[f"{folder}/module.o"],
        cmd=[wasm2o.outputs[0], "-o", f"{folder}/module.o", wasm.outputs[0]],
        descr="AO", color="light-cyan",
    )
    wrapper = fetch(f"stand_{kind}_source", f"https://raw.githubusercontent.com/pg83/ix/{DECODE_IX}/pkgs/{source}", source_digest, f"{folder}/wrapper" + os.path.splitext(source)[1])
    native = command(
        name=f"stand_{kind}_native",
        inputs=["$(S)/dev/stand/native.py", "$(S)/dev/stand/native.h", "$(S)/dev/stand/decode.c"],
        deps=[wrapper],
        outputs=[f"{folder}/native"],
        cmd=["python3", "$(S)/dev/stand/native.py", kind, wrapper.outputs[0], f"{folder}/native", NATIVE_CONFIG],
        env={"PKG_CONFIG_PATH": os.environ.get("PKG_CONFIG_PATH", "")},
        descr="CC", color="green",
    )
    decode_stand.extend([obj, native])

# CPython built natively from the tarball python.wasm comes from, with its
# built-in modules, against python.o on dev/stand/pybench.py.
stand_python_native = command(
    name="stand_python_native",
    inputs=["$(S)/dev/stand/python.py", "$(S)/bin/python/make.py", "$(S)/dev/wasm.py"],
    deps=[python_tarball],
    outputs=["$(B)/stand/python/native"],
    cmd=["python3", "$(S)/dev/stand/python.py", python_tarball.outputs[0], "$(B)/stand/python/native", NATIVE_CONFIG],
    descr="CC", color="green",
)

stand_corpus = fetch(
    "stand_corpus",
    "https://raw.githubusercontent.com/pg83/store/1a4411b2ee6fdf6c3a05b7ddafd4d71ab79dbb54/decode-corpus-1.tgz",
    "0d6a8975159dd12a0a8d6bee270659791780bef806151790054205326c6d2685",
    "$(B)/stand/decode-corpus-1.tgz",
)

# What each test runs against: its arguments are the outputs of these nodes.
TESTED = {
    "aot": [aot, aot_command, wazero, wasm2o, busybox, aot_selftest],
    "busybox_abi": [busybox],
    "busybox": [wasm2o, busybox_o, busybox],
    "clang": [wasm2o, clang_o, clang, libc, libc_headers, libcxx],
    "cxx": [wasm2o, cxx_check_o],
    "doom": [wasm2o, doom_o, doom_wad],
    "libc": [wasm2o, libc_check_o],
    "python": [wasm2o, python_o, python, python_tarball, busybox_o, busybox],
    "wasm2o": [wasm2o, aot_native, aot, aot_command, aot_command_aot, aot_relinked, wazero, busybox],
}
tests = []

for test in build.glob("$(S)/tst/test_*.py"):
    name = test.rsplit("/", 1)[-1][len("test_"):-len(".py")]
    stamp = f"$(B)/tst/{name}.stamp"
    tested = TESTED[name]
    tests.append(command(
        name=f"test_{name}",
        inputs=[test, "$(S)/dev/wasm_info.py"],
        outputs=[stamp],
        deps=tested,
        cmd=[
            ["python3", test, *(output for node in tested for output in node.outputs)],
            touch(stamp),
        ],
        env={"PYTHONDONTWRITEBYTECODE": "1"},
        descr="TS",
        color="green",
    ))

group("install", libcxx, libc_headers, clang, clang_o, busybox, busybox_abi, busybox_o, doom, doom_o, python, python_o, wasm2o, aot_native, aot, aot_aot, wazero)
group("test", *tests, wasm2o_go_test, wasm2o_spec_test)
group("stand", wasm2o, aot_native, aot, aot_aot, busybox, stand_corpus, *decode_stand, python, python_o, stand_python_native)
