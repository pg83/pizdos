import build
import json
import os
import re
import shlex
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
# compiler in the convention of the code vm compiles, for its loader.
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
# vm, so a change of the compiler rebuilds none of it.
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
# runtime metadata when vm embeds the compiled reactor. The multivalue
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

# A native build needs host libc headers/libraries, supplied by the usual
# CPPFLAGS/LDFLAGS. The C driver links libc without a C++ runtime.
aot_native = command(
    name="aot_native",
    inputs=["$(S)/bin/aot/aot.cpp"],
    outputs=["$(B)/bin/aot/aot"],
    cmd=[CLANG, *build.cppflags, *build.cxxflags, *AOT_FLAGS, "-O3", "$(S)/bin/aot/aot.cpp", "-o", "$(B)/bin/aot/aot", *build.ldflags],
    descr="CC", color="green",
)

coverage_profile_runtime = os.environ.get("COVERAGE_PROFILE_RUNTIME", "")
coverage_native_inputs = ["$(S)/bin/aot/aot.cpp", "$(S)/dev/coverage_build.py", "$(S)/dev/coverage_native.c", *([coverage_profile_runtime] if coverage_profile_runtime else [])]
coverage_cppflags = shlex.split(os.environ["COVERAGE_CPPFLAGS"]) if "COVERAGE_CPPFLAGS" in os.environ else build.cppflags
coverage_ldflags = shlex.split(os.environ["COVERAGE_LDFLAGS"]) if "COVERAGE_LDFLAGS" in os.environ else build.ldflags
coverage_native_tools = {"cc": CLANG, "cppflags": coverage_cppflags, "cxxflags": build.cxxflags, "ldflags": coverage_ldflags, "flags": AOT_FLAGS, "profile_runtime": coverage_profile_runtime}
aot_coverage = command(
    name="aot_coverage",
    inputs=coverage_native_inputs,
    outputs=["$(B)/coverage/bin/aot"],
    cmd=["python3", "$(S)/dev/coverage_build.py", "$(S)/bin/aot/aot.cpp", "$(S)/dev/coverage_native.c",
         "$(B)/coverage/bin/aot", json.dumps(coverage_native_tools)],
    descr="CC", color="green",
)

aot_chaos_coverage = command(
    name="aot_chaos_coverage",
    inputs=coverage_native_inputs,
    outputs=["$(B)/coverage/chaos/aot"],
    cmd=["python3", "$(S)/dev/coverage_build.py", "$(S)/bin/aot/aot.cpp", "$(S)/dev/coverage_native.c",
         "$(B)/coverage/chaos/aot", json.dumps({**coverage_native_tools, "chaos": True})],
    descr="CC", color="green",
)

aot_measure = command(
    name="aot_measure",
    inputs=["$(S)/dev/aot_measure.c"],
    outputs=["$(B)/dev/aot_measure"],
    cmd=[CLANG, *build.cppflags, "-O2", "$(S)/dev/aot_measure.c", "-o", "$(B)/dev/aot_measure", *build.ldflags],
    descr="CC", color="green",
)

# Bootstrap without an existing vm: the native C++ compiler compiles the
# WASI reactor to the machine-code object embedded in the Go loader.
aot_aot = command(
    name="aot_aot",
    inputs=["$(S)/dev/aot.py"],
    outputs=["$(B)/bin/aot/aot.aot"],
    deps=[aot_native, aot],
    cmd=["python3", "$(S)/dev/aot.py", aot_native.outputs[0], aot.outputs[0], "$(B)/bin/aot/aot.aot"],
    descr="AO", color="light-cyan",
)

aot_native_chaos = command(
    name="aot_native_chaos",
    inputs=["$(S)/bin/aot/aot.cpp"],
    outputs=["$(B)/chaos/bin/aot"],
    cmd=[CLANG, *build.cppflags, *build.cxxflags, *AOT_FLAGS, "-O3", "-DAOT_CHAOS", "$(S)/bin/aot/aot.cpp",
         "-o", "$(B)/chaos/bin/aot", *build.ldflags],
    descr="CC", color="green",
)

aot_wasm_chaos = command(
    name="aot_wasm_chaos",
    inputs=["$(S)/bin/aot/aot.cpp"],
    outputs=["$(B)/chaos/bin/aot.wasm"],
    cmd=[CLANG, *AOT_WASM_FLAGS, "-DAOT_CHAOS", "$(S)/bin/aot/aot.cpp", "-o", "$(B)/chaos/bin/aot.wasm"],
    descr="CC", color="green",
)

aot_chaos_aot = command(
    name="aot_chaos_aot",
    inputs=["$(S)/dev/aot.py"],
    outputs=["$(B)/chaos/compiler/aot.aot"],
    deps=[aot_native, aot_wasm_chaos],
    cmd=["python3", "$(S)/dev/aot.py", aot_native.outputs[0], aot_wasm_chaos.outputs[0], "$(B)/chaos/compiler/aot.aot"],
    descr="AO", color="light-cyan",
)

# Keep LLVM's coverage mapping in the Wasm module. Only coverage VMs embed
# these reactors; the production and chaos executables use the usual ones.
wasm_coverage = []
for chaos in (False, True):
    variant = "chaos/" if chaos else ""
    prefix = f"$(B)/coverage/{variant}compiler"
    reactor = command(
        name="aot_wasm_chaos_coverage" if chaos else "aot_wasm_coverage",
        inputs=["$(S)/bin/aot/aot.cpp", "$(S)/dev/coverage_wasm.c", "$(S)/dev/coverage_wasm_build.py"],
        outputs=[prefix + "/aot.wasm"],
        cmd=["python3", "$(S)/dev/coverage_wasm_build.py", CLANG, WASM_LD,
             "$(S)/bin/aot/aot.cpp", "$(S)/dev/coverage_wasm.c", prefix + "/aot.wasm",
             json.dumps({"flags": AOT_WASM_FLAGS, "chaos": chaos})],
        descr="CC", color="green",
    )
    embedded = command(
        name="aot_chaos_coverage_aot" if chaos else "aot_coverage_aot",
        inputs=["$(S)/dev/aot.py"],
        outputs=[prefix + "/aot.aot"],
        deps=[aot_native, reactor],
        cmd=["python3", "$(S)/dev/aot.py", aot_native.outputs[0], reactor.outputs[0], prefix + "/aot.aot"],
        descr="AO", color="light-cyan",
    )
    wasm_coverage.append((reactor, embedded))

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

vm = command(
    name="vm",
    inputs=[*WASM2O_SOURCES, *GO_RUNTIME],
    outputs=["$(B)/bin/vm/vm"],
    deps=[aot_aot, libstr],
    cmd=["python3", "$(S)/dev/go.py", "build", GO, "$(S)/bin/vm", "$(B)/bin/vm/vm", "--runtime", "$(S)/lib/goruntime", "--embed", aot_aot.outputs[0],
         "--embed", libstr.outputs[0]],
    descr="GO", color="light-blue",
)

runtime_abi_vms = []
for mode in ("version", "layout"):
    output = f"$(B)/tst/runtime_abi/{mode}"
    runtime_abi_vms.append(command(
        name=f"vm_runtime_{mode}",
        inputs=[*WASM2O_SOURCES, *GO_RUNTIME, "$(S)/tst/runtime_abi/make.py"],
        outputs=[output],
        deps=[aot_aot, libstr],
        cmd=["python3", "$(S)/tst/runtime_abi/make.py", GO, "$(S)", mode,
             aot_aot.outputs[0], libstr.outputs[0], output],
        descr="GO", color="light-blue",
    ))

vm_coverage = command(
    name="vm_coverage",
    inputs=[*WASM2O_SOURCES, *GO_RUNTIME, "$(S)/dev/coverage_collect.go"],
    outputs=["$(B)/coverage/bin/vm"],
    deps=[wasm_coverage[0][1], libstr],
    cmd=["python3", "$(S)/dev/go.py", "build", GO, "$(S)/bin/vm", "$(B)/coverage/bin/vm",
         "--cover", "--tags", "pizdoscoverage", "--runtime", "$(S)/lib/goruntime",
         "--embed", wasm_coverage[0][1].outputs[0], "--embed", libstr.outputs[0], "--embed", "$(S)/dev/coverage_collect.go"],
    descr="GO", color="light-blue",
)

vm_chaos = command(
    name="vm_chaos",
    inputs=[*WASM2O_SOURCES, *GO_RUNTIME],
    outputs=["$(B)/chaos/bin/vm"],
    deps=[aot_chaos_aot, libstr],
    cmd=["python3", "$(S)/dev/go.py", "build", GO, "$(S)/bin/vm", "$(B)/chaos/bin/vm", "--tags", "pizdoschaos",
         "--runtime", "$(S)/lib/goruntime", "--embed", aot_chaos_aot.outputs[0], "--embed", libstr.outputs[0]],
    descr="GO", color="light-blue",
)

vm_chaos_coverage = command(
    name="vm_chaos_coverage",
    inputs=[*WASM2O_SOURCES, *GO_RUNTIME, "$(S)/dev/coverage_collect.go"],
    outputs=["$(B)/coverage/chaos/vm"],
    deps=[wasm_coverage[1][1], libstr],
    cmd=["python3", "$(S)/dev/go.py", "build", GO, "$(S)/bin/vm", "$(B)/coverage/chaos/vm", "--cover", "--tags", "pizdoschaos,pizdoscoverage",
         "--runtime", "$(S)/lib/goruntime", "--embed", wasm_coverage[1][1].outputs[0], "--embed", libstr.outputs[0],
         "--embed", "$(S)/dev/coverage_collect.go"],
    descr="GO", color="light-blue",
)

# busybox.wasm compiled by vm: what `vm run` runs in the kernel's
# processes.
busybox_o = command(
    name="busybox_o",
    outputs=["$(B)/bin/busybox/busybox.o"],
    deps=[vm, busybox],
    cmd=[vm.outputs[0], "-o", "$(B)/bin/busybox/busybox.o", busybox.outputs[0]],
    descr="AO",
    color="light-cyan",
)

doom_o = command(
    name="doom_o",
    outputs=["$(B)/bin/doom/doom.o"],
    deps=[vm, doom],
    cmd=[vm.outputs[0], "-o", "$(B)/bin/doom/doom.o", doom.outputs[0]],
    descr="AO",
    color="light-cyan",
)

# python.wasm, its modules frozen by the freezer running in the kernel, and
# the standard library it imports from a zip.
python = command(
    name="python",
    inputs=[*LIBC_HEADERS, "$(S)/bin/python/pyconfig.h", "$(S)/bin/python/make.py", "$(S)/dev/wasm.py"],
    outputs=["$(B)/bin/python/python.wasm", "$(B)/bin/python/python314.zip"],
    deps=[python_tarball, libc, python_core, vm],
    cmd=[
        "python3", "$(S)/bin/python/make.py", "python",
        CLANG, WASM_LD, vm.outputs[0],
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
    deps=[vm, python],
    cmd=[vm.outputs[0], "-o", "$(B)/bin/python/python.o", python.outputs[0]],
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
    deps=[vm, clang],
    cmd=[vm.outputs[0], "-o", "$(B)/bin/clang/llvm.o", clang.outputs[0]],
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
    deps=[vm, libc_check],
    cmd=[vm.outputs[0], "-o", "$(B)/tst/libc/check.o", libc_check.outputs[0]],
    descr="AO",
    color="light-cyan",
)

# test_asan's program: lib/c's allocator, string functions and calls misused,
# one memory error a case, built as test_libc's is but with -g, so the
# object's traces name the lines, and compiled with --asan.
asan_check = command(
    name="asan_check",
    inputs=[*LIBC_HEADERS, "$(S)/tst/asan/check.c", "$(S)/tst/libc/make.py", "$(S)/dev/wasm.py"],
    outputs=["$(B)/tst/asan/check.wasm"],
    deps=[libc],
    cmd=["python3", "$(S)/tst/libc/make.py", CLANG, WASM_LD, "$(S)/tst/asan/check.c", "$(S)/lib/c/include", libc.outputs[0], "$(B)/tst/asan/check.wasm", "-g"],
    descr="CC",
    color="green",
)

asan_check_o = command(
    name="asan_check_o",
    outputs=["$(B)/tst/asan/check.o"],
    deps=[vm, asan_check],
    cmd=[vm.outputs[0], "--asan", "-o", "$(B)/tst/asan/check.o", asan_check.outputs[0]],
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
    deps=[vm, cxx_check],
    cmd=[vm.outputs[0], "-o", "$(B)/tst/cxx/check.o", cxx_check.outputs[0]],
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

# The WebAssembly spec testsuite through `vm spec`; wast2json comes from
# wabt, under ix run bld/wabt.
TESTSUITE = "9b9fbf8260507169066bf3e8de7b01fb8c145e55"
WAST2JSON = shutil.which("wast2json") or ""
WAT2WASM = shutil.which("wat2wasm") or ""

testsuite_tarball = fetch(
    "testsuite_tarball",
    f"https://github.com/WebAssembly/testsuite/archive/{TESTSUITE}.tar.gz",
    "0d0ed50f55f17f25b5db9e28d3d84d4a0a9feb5e97fd9a718f6d7017b16eb2e4",
    f"$(B)/ext/testsuite-{TESTSUITE}.tar.gz",
)

vm_spec_test = command(
    name="test_vm_spec",
    inputs=["$(S)/tst/vm_spec.py"],
    outputs=["$(B)/tst/vm_spec.stamp"],
    deps=[vm, aot_native, testsuite_tarball],
    cmd=["python3", "$(S)/tst/vm_spec.py", vm.outputs[0], WAST2JSON, testsuite_tarball.outputs[0], "$(B)/tst/vm_spec.stamp", aot_native.outputs[0]],
    descr="TS",
    color="green",
)

# tst/wazero: a WASI runner on wazero, what vm is measured against. Its
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

# The stand compares vm's code with native code of the same programs:
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
        deps=[vm, wasm],
        outputs=[f"{folder}/module.o"],
        cmd=[vm.outputs[0], "-o", f"{folder}/module.o", wasm.outputs[0]],
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

stand_fixture = command(
    name="stand_fixture",
    inputs=["$(S)/tst/stand/fixture.cpp"],
    outputs=["$(B)/tst/stand/fixture.wasm"],
    cmd=[CLANG, *AOT_WASM_FLAGS, "$(S)/tst/stand/fixture.cpp", "-o", "$(B)/tst/stand/fixture.wasm"],
    descr="CC", color="green",
)

# What each test runs against: its arguments are the outputs of these nodes.
chaos_check = command(
    name="chaos_check",
    inputs=[*LIBC_HEADERS, "$(S)/tst/chaos/check.c", "$(S)/tst/libc/make.py", "$(S)/dev/wasm.py"],
    outputs=["$(B)/tst/chaos/check.wasm"],
    deps=[libc],
    cmd=["python3", "$(S)/tst/libc/make.py", CLANG, WASM_LD, "$(S)/tst/chaos/check.c", "$(S)/lib/c/include", libc.outputs[0], "$(B)/tst/chaos/check.wasm", "-g"],
    descr="CC", color="green",
)

terminal_check = command(
    name="terminal_check",
    inputs=[*LIBC_HEADERS, "$(S)/tst/terminal/check.c", "$(S)/tst/libc/make.py", "$(S)/dev/wasm.py"],
    outputs=["$(B)/tst/terminal/check.wasm"],
    deps=[libc],
    cmd=["python3", "$(S)/tst/libc/make.py", CLANG, WASM_LD, "$(S)/tst/terminal/check.c", "$(S)/lib/c/include", libc.outputs[0], "$(B)/tst/terminal/check.wasm", "-g"],
    descr="CC", color="green",
)

host_resources_check = command(
    name="host_resources_check",
    inputs=[*LIBC_HEADERS, "$(S)/tst/host_resources/check.c", "$(S)/tst/libc/make.py", "$(S)/dev/wasm.py"],
    outputs=["$(B)/tst/host_resources/check.wasm"],
    deps=[libc],
    cmd=["python3", "$(S)/tst/libc/make.py", CLANG, WASM_LD, "$(S)/tst/host_resources/check.c", "$(S)/lib/c/include", libc.outputs[0], "$(B)/tst/host_resources/check.wasm", "-g", "-D_GNU_SOURCE"],
    descr="CC", color="green",
)

host_syscalls_check = command(
    name="host_syscalls_check",
    inputs=[*LIBC_HEADERS, "$(S)/tst/host_syscalls/check.c", "$(S)/tst/libc/make.py", "$(S)/dev/wasm.py"],
    outputs=["$(B)/tst/host_syscalls/check.wasm"],
    deps=[libc],
    cmd=["python3", "$(S)/tst/libc/make.py", CLANG, WASM_LD, "$(S)/tst/host_syscalls/check.c", "$(S)/lib/c/include", libc.outputs[0], "$(B)/tst/host_syscalls/check.wasm", "-g", "-D_GNU_SOURCE"],
    descr="CC", color="green",
)

host_syscalls_filter = command(
    name="host_syscalls_filter",
    inputs=["$(S)/tst/host_syscalls/filter.c"],
    outputs=["$(B)/tst/host_syscalls/filter"],
    cmd=[CLANG, *build.cppflags, *build.cflags, "-O2", "$(S)/tst/host_syscalls/filter.c", "-o", "$(B)/tst/host_syscalls/filter", *build.ldflags],
    descr="CC", color="green",
)

host_syscalls_interrupt = command(
    name="host_syscalls_interrupt",
    inputs=["$(S)/tst/host_syscalls/interrupt.c"],
    outputs=["$(B)/tst/host_syscalls/interrupt"],
    cmd=[CLANG, *build.cppflags, *build.cflags, "-O2", "$(S)/tst/host_syscalls/interrupt.c", "-o", "$(B)/tst/host_syscalls/interrupt", *build.ldflags],
    descr="CC", color="green",
)

E2E_TESTED = {
    "aot": [aot, aot_command, wazero, vm, chaos_check],
    "asan": [vm, asan_check_o, libc_check_o],
    "asan_bounds": [vm, aot_native, wazero],
    "bounded_copy_loops": [vm, aot_native, wazero],
    "chaos": [vm, vm_chaos, aot_native, aot_native_chaos, chaos_check],
    "cli_errors": [vm, aot_native],
    "compiler_modes": [vm, aot_native],
    "compiler_rejections": [vm, aot_native, wazero],
    "conditional_floats": [vm, aot_native, wazero],
    "constant_arithmetic": [vm, aot_native, wazero],
    "copy_loops": [vm, aot_native],
    "cxx": [vm, cxx_check_o],
    "data_segments": [vm, aot_native],
    "dead_instructions": [vm, aot_native, wazero],
    "deep_control": [vm, aot_native, wazero],
    "dispatch_loops": [vm, aot_native, wazero],
    "fork_modes": [vm, aot_native, wazero],
    "float_boundaries": [vm, aot_native, wazero],
    "float_integer_phis": [vm, aot_native, wazero],
    "float_permutations": [vm, aot_native, wazero],
    "debug_lines": [vm, aot_native],
    "debug_names": [vm, aot_native],
    "import_abi": [vm, aot_native],
    "import_errors": [vm, aot_native, wazero],
    "initializers": [vm, aot_native, wazero],
    "interleaved_float_selects": [vm, aot_native, wazero],
    "integer_identities": [vm, aot_native, wazero],
    "kernel_exhaustion": [vm, aot_native, wazero],
    "invariant_loops": [vm, aot_native, wazero],
    "invariant_results": [vm, aot_native, wazero],
    "host_io": [vm, aot_native],
    "host_resources": [vm, aot_native, host_resources_check],
    "host_syscalls": [vm, aot_native, host_syscalls_check, host_syscalls_filter, host_syscalls_interrupt],
    "runtime_abi": [vm, aot_native, *runtime_abi_vms],
    "host_signals": [vm, aot_native, wazero],
    "libc": [vm, libc_check_o],
    "large_functions": [vm, aot_native],
    "large_switches": [vm, aot_native, wazero],
    "loop_limits": [vm, aot_native, wazero],
    "objects": [vm, aot_native],
    "profile_artifacts": [vm, aot_native],
    "runtime_errors": [vm, aot_native],
    "runtime_safety": [vm, aot_native, wazero],
    "spec_cli": [vm, aot_native],
    "sparse_values": [vm, aot_native, wazero],
    "stand": [vm, aot_native, stand_fixture],
    "terminal": [vm, aot_native, terminal_check, host_syscalls_filter],
    "table_defaults": [vm, aot_native, wazero],
    "unary_loops": [vm, aot_native, wazero],
    "vfork_traps": [vm, aot_native, wazero],
    "wide_results": [vm, aot_native, wazero],
    "vm": [vm, aot_native, aot, aot_command, aot_command_aot, aot_relinked, wazero, chaos_check],
}
APPLICATION_TESTED = {
    "busybox_abi": [busybox],
    "busybox": [vm, busybox_o, busybox],
    "clang": [vm, clang_o, clang, libc, libc_headers, libcxx],
    "doom": [vm, doom_o, doom_wad],
    "python": [vm, python_o, python, python_tarball, busybox_o, busybox],
}
TESTED = {**E2E_TESTED, **APPLICATION_TESTED}
tests = []
e2e_tests = []

UNIT_TOOLS = json.dumps({
    "cc": CLANG,
    "ld": WASM_LD,
    "aot": aot_native.outputs[0],
    "vm": vm.outputs[0],
    "c_headers": "$(S)/lib/c/include",
    "libc": libc.outputs[0],
    "cxx_headers": libcxx.outputs[1],
    "libcxx": libcxx.outputs[0],
    "host_cflags": [*build.cppflags, *build.cxxflags],
    "host_ldflags": build.ldflags,
}, sort_keys=True)
unit_tests = []
coverage_cases = []
coverage_dependencies = []
coverage_inputs = ["$(S)/dev/coverage.py", "$(S)/dev/coverage_wasm.py", "$(S)/tst/unit/run.py", "$(S)/dev/wasm.py", "$(S)/dev/wasm_info.py"]
coverage_filter = os.environ.get("COVERAGE_TESTS", "")


def coverage_case(name, command_line, inputs, dependencies):
    if coverage_filter and not re.search(coverage_filter, name):
        return
    coverage_cases.append({"name": name, "command": command_line})
    coverage_inputs.extend(inputs)
    coverage_dependencies.extend(dependencies)

for source in build.glob("$(S)/tst/unit/**/*.cpp"):
    name = source[len("$(S)/tst/unit/"):-len(".cpp")]
    prefix = f"$(B)/tst/unit/{name}"
    unit_tests.append(command(
        name="unit_" + name.replace("/", "_"),
        inputs=[source, "$(S)/tst/unit/run.py", "$(S)/dev/wasm.py", *LIBC_HEADERS],
        outputs=[prefix + suffix for suffix in (".stamp", ".wasm", ".native.o", ".embedded.o")],
        deps=[aot_native, vm, libc, libcxx],
        cmd=["python3", "$(S)/tst/unit/run.py", source, prefix, UNIT_TOOLS],
        env={"PYTHONDONTWRITEBYTECODE": "1"},
        descr="EE", color="green",
    ))
    coverage_tools = json.loads(UNIT_TOOLS)
    coverage_tools.update(vm="$VM", aot="$AOT")
    coverage_case("unit_" + name.replace("/", "_"),
                  ["python3", "$(S)/tst/unit/run.py", source, "$OUT", json.dumps(coverage_tools)],
                  [source, *LIBC_HEADERS], [libc, libcxx])

for test in build.glob("$(S)/tst/test_*.py"):
    name = test.rsplit("/", 1)[-1][len("test_"):-len(".py")]
    stamp = f"$(B)/tst/{name}.stamp"
    tested = TESTED[name]
    case_test = command(
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
    )
    tests.append(case_test)
    if name not in E2E_TESTED:
        continue
    e2e_tests.append(case_test)
    coverage_arguments = []
    for node in tested:
        if node is vm:
            coverage_arguments.append("$VM")
        elif node is aot_native:
            coverage_arguments.append("$AOT")
        elif node is vm_chaos:
            coverage_arguments.append("$VM_CHAOS")
        elif node is aot_native_chaos:
            coverage_arguments.append("$AOT_CHAOS")
        else:
            coverage_arguments.extend(node.outputs)
    coverage_case("test_" + name, ["python3", test, *coverage_arguments], [test],
                  [node for node in tested if node not in (vm, aot_native, vm_chaos, aot_native_chaos)])

for name, runner, fixture in (("wat_updates", "run_updates.py", "update_branches.wat"),
                              ("wat_float_selects", "run_float_selects.py", "float_selects.wat"),
                              ("wat_memory_arithmetic", "run_memory_arithmetic.py", "memory_arithmetic.wat"),
                              ("wat_copy_addresses", "run_copy_addresses.py", "copy_addresses.wat"),
                              ("wat_dispatch_values", "run_dispatch_values.py", "dispatch_values.wat"),
                              ("wat_go_float_results", "run_go_float_results.py", "go_float_results.wat"),
                              ("wat_deep_addresses", "run_deep_addresses.py", "deep_addresses.wat"),
                              ("wat_result_masks", "run_result_masks.py", "result_masks.wat"),
                              ("wat_exit_values", "run_exit_values.py", "exit_values.wat"),
                              ("wat_startup_fork", "run_startup_fork.py", "startup_fork.wat"),
                              ("wat_branch_threads", "run_branch_threads.py", "branch_threads.wat"),
                              ("wat_branch_pairs", "run_branch_pairs.py", "branch_pairs.wat"),
                              ("wat_diamond_lifetimes", "run_diamond_lifetimes.py", "diamond_lifetimes.wat"),
                              ("wat_conversion_diamonds", "run_conversion_diamonds.py", "conversion_diamonds.wat"),
                              ("wat_threaded_parameters", "run_threaded_parameters.py", "threaded_parameters.wat"),
                              ("wat_late_constants", "run_late_constants.py", "late_constants.wat"),
                              ("wat_shared_addresses", "run_shared_addresses.py", "shared_addresses.wat"),
                              ("wat_address_rotation", "run_address_rotation.py", "address_rotation.wat"),
                              ("wat_adjacent_loops", "run_adjacent_loops.py", "adjacent_loops.wat"),
                              ("wat_wide_allocation_limit", "run_wide_allocation_limit.py", "wide_allocation_limit.wat"),
                              ("wat_loop_escapes", "run_loop_escapes.py", "loop_escapes.wat")):
    runner, fixture = "$(S)/tst/wat/" + runner, "$(S)/tst/wat/" + fixture
    stamp = f"$(B)/tst/{name}.stamp"
    case_test = command(
        name="test_" + name,
        inputs=[runner, fixture],
        outputs=[stamp],
        deps=[vm, aot_native, wazero],
        cmd=[["python3", runner, vm.outputs[0], aot_native.outputs[0], wazero.outputs[0], WAT2WASM, fixture], touch(stamp)],
        descr="EE", color="green",
    )
    tests.append(case_test)
    e2e_tests.append(case_test)
    coverage_case("test_" + name, ["python3", runner, "$VM", "$AOT", wazero.outputs[0], WAT2WASM, fixture],
                  [runner, fixture], [wazero])

coverage_case("test_vm_spec",
              ["python3", "$(S)/tst/vm_spec.py", "$VM", WAST2JSON, testsuite_tarball.outputs[0], "$OUT.stamp", "$AOT"],
              ["$(S)/tst/vm_spec.py"], [testsuite_tarball])

coverage = command(
    name="coverage",
    inputs=[*dict.fromkeys(coverage_inputs), *WASM2O_SOURCES, "$(S)/bin/aot/aot.cpp"],
    outputs=["$(B)/coverage/" + name for name in ("summary.json", "summary.txt", "uncovered.txt", "aot.json", "aot-chaos.json", "vm.out", "vm-codecov.out", "aot-native.info", "aot-wasm.info")]
            + ["$(B)/coverage/" + stem + suffix for stem in ("aot-native", "aot-native-chaos") for suffix in (".profdata", ".lcov")]
            + ["$(B)/coverage/" + stem + suffix for stem in ("aot-wasm", "aot-wasm-chaos") for suffix in (".proftext", ".profdata", ".lcov")]
            + ["$(B)/coverage/" + case["name"] + ".log" for case in coverage_cases],
    deps=[aot_coverage, vm_coverage, aot_chaos_coverage, vm_chaos_coverage,
          *[reactor for reactor, embedded in wasm_coverage], *dict.fromkeys(coverage_dependencies)],
    cmd=["python3", "$(S)/dev/coverage.py", "$(B)/coverage", json.dumps({
        "jobs": int(os.environ.get("COVERAGE_JOBS") or os.environ.get("BUILD_JOBS") or "4"),
        "wrapper_tools": {"cc": CLANG, "cppflags": build.cppflags, "ldflags": build.ldflags},
        "source_root": "$(S)", "aot": aot_coverage.outputs[0], "vm": vm_coverage.outputs[0],
        "aot_chaos": aot_chaos_coverage.outputs[0], "vm_chaos": vm_chaos_coverage.outputs[0],
        "go": GO,
        "aot_wasm": wasm_coverage[0][0].outputs[0], "aot_wasm_chaos": wasm_coverage[1][0].outputs[0],
        "llvm_cov": os.environ.get("LLVM_COV") or shutil.which("llvm-cov") or "llvm-cov",
        "llvm_profdata": os.environ.get("LLVM_PROFDATA") or shutil.which("llvm-profdata") or "llvm-profdata",
        "cases": coverage_cases,
    })],
    descr="CV", color="cyan",
)

coverage_check = command(
    name="coverage_check",
    inputs=["$(S)/dev/coverage.py"],
    outputs=["$(B)/coverage/check.stamp"],
    deps=[coverage],
    cmd=[["python3", "$(S)/dev/coverage.py", "--check", coverage.outputs[0], "100"], touch("$(B)/coverage/check.stamp")],
    descr="CV", color="cyan",
)

group("install", libcxx, libc_headers, clang, clang_o, busybox, busybox_abi, busybox_o, doom, doom_o, python, python_o, vm, aot_native, aot, aot_aot, wazero)
group("unit", *unit_tests)
group("e2e", *e2e_tests, *unit_tests, vm_spec_test)
group("test", *tests, *unit_tests, vm_spec_test)
group("stand", vm, aot_native, aot, aot_aot, busybox, stand_corpus, *decode_stand, python, python_o, stand_python_native)
