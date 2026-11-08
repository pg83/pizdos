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
LLVM_AR = tool("llvm-ar")
WASM_LD = tool("wasm-ld")
LD_LLD = tool("ld.lld")
GO = tool("go")

MUSL = "1.2.5"
BUSYBOX = "1.36.1"


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

# bin/wasm2o: the compiler, a Go program with no dependencies, built
# natively, its loader and kernel with our fork of Go's runtime
# (lib/goruntime), and as a WASI module.
WASM2O_SOURCES = [*build.glob("$(S)/bin/wasm2o/*.go"), "$(S)/bin/wasm2o/go.mod", "$(S)/dev/go.py"]
GO_RUNTIME = [*build.glob("$(S)/lib/goruntime/*.go"), "$(S)/lib/goruntime/edits.py"]

wasm2o = command(
    name="wasm2o",
    inputs=[*WASM2O_SOURCES, *GO_RUNTIME],
    outputs=["$(B)/bin/wasm2o/wasm2o"],
    cmd=["python3", "$(S)/dev/go.py", "build", GO, "$(S)/bin/wasm2o", "$(B)/bin/wasm2o/wasm2o", "--runtime", "$(S)/lib/goruntime"],
    descr="GO",
    color="light-blue",
)

wasm2o_wasm = command(
    name="wasm2o_wasm",
    inputs=WASM2O_SOURCES,
    outputs=["$(B)/bin/wasm2o/wasm2o.wasm"],
    cmd=["python3", "$(S)/dev/go.py", "build", GO, "$(S)/bin/wasm2o", "$(B)/bin/wasm2o/wasm2o.wasm", "--wasip1"],
    descr="GO",
    color="light-blue",
)

# bin/aot is a single freestanding C++ translation unit. The WASI reactor
# exports compile(Blob) -> Blob; the command build also supplies _start.
AOT_FLAGS = ["-x", "c++", "-std=c++20", "-O2", "-fno-exceptions", "-fno-rtti", "-fno-builtin", "-fwrapv", "-ffp-contract=off", "-Wno-parentheses-equality"]
AOT_WASM_FLAGS = [*AOT_FLAGS, "--target=wasm32-unknown-unknown", "-ffreestanding", "-nostdlib", "-nostdinc", "-nostdinc++", "-mbulk-memory", "-Wl,--no-entry", "-Wl,--export-memory", "-Wl,-z,stack-size=4194304"]

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
    cmd=[CLANG, *build.cppflags, *build.cxxflags, *AOT_FLAGS, "$(S)/bin/aot/aot.cpp", "-o", "$(B)/bin/aot/aot", *build.ldflags],
    descr="CC", color="green",
)

# wasm2o.wasm compiled by wasm2o: the compiler as its own output, which
# `wasm2o run` executes on a goroutine stack.
wasm2o_o = command(
    name="wasm2o_o",
    outputs=["$(B)/bin/wasm2o/wasm2o.o"],
    deps=[wasm2o, wasm2o_wasm],
    cmd=[wasm2o.outputs[0], "-o", "$(B)/bin/wasm2o/wasm2o.o", wasm2o_wasm.outputs[0]],
    descr="AO",
    color="light-cyan",
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

# wasm2o.o through an ordinary linker: ld.lld -r combines and relocates it
# again, and the loader runs what comes out.
wasm2o_relinked = command(
    name="wasm2o_relinked",
    outputs=["$(B)/bin/wasm2o/wasm2o.relinked.o"],
    deps=[wasm2o_o],
    cmd=[LD_LLD, "-r", wasm2o_o.outputs[0], "-o", "$(B)/bin/wasm2o/wasm2o.relinked.o"],
    descr="LD",
    color="green",
)

# wasm2o's own tests: the loader drives Go's runtime through native frames.
wasm2o_go_test = command(
    name="test_wasm2o_go",
    inputs=[*build.glob("$(S)/bin/wasm2o/*.go"), "$(S)/bin/wasm2o/go.mod", "$(S)/dev/go.py", *GO_RUNTIME],
    outputs=["$(B)/tst/wasm2o_go.stamp"],
    cmd=["python3", "$(S)/dev/go.py", "test", GO, "$(S)/bin/wasm2o", "$(B)/tst/wasm2o_go.stamp", "--runtime", "$(S)/lib/goruntime"],
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

# The stand: wasm2o.wasm as wasm2c and clang make it, in the mode that
# matches wasm2o's code (explicit bounds checks, memory reserved with mmap
# and grown in place, a counted call depth) and at its best otherwise: -O3,
# ThinLTO across the parts, no frame pointer. Its host is dev/stand. It
# takes a host C library, `ix run lib/c bld/compiler bld/wabt -- ./build
# stand_bin`; the full stand below also adds native decoder libraries.
WASM2C = shutil.which("wasm2c") or ""
WASM2C_RT = os.path.join(os.path.dirname(WASM2C), "../share/wabt/wasm2c") if WASM2C else ""
STAND_PARTS = 16
STAND_HOST = ["$(S)/dev/stand/host_w2c.c", "$(S)/dev/stand/wasi.c", "$(S)/dev/stand/wasi.h"]
STAND_RT = [f"{WASM2C_RT}/wasm-rt-impl.c", f"{WASM2C_RT}/wasm-rt-mem-impl.c", f"{WASM2C_RT}/wasm-rt-exceptions-impl.c"]
STAND_FLAGS = [
    *build.cppflags, *build.cflags,
    "-O3", "-march=x86-64-v3", "-fomit-frame-pointer", "-flto=thin",
    "-DWASM_RT_MEMCHECK_BOUNDS_CHECK=1", "-DWASM_RT_USE_MMAP=1",
    "-DWASM_RT_STACK_DEPTH_COUNT=1", "-DWASM_RT_MAX_CALL_STACK_DEPTH=100000000",
    "-I" + WASM2C_RT, "-I$(B)/stand",
]

stand_src = command(
    name="stand_src",
    deps=[wasm2o_wasm],
    outputs=["$(B)/stand/prog.h", "$(B)/stand/prog-impl.h", *[f"$(B)/stand/prog_{i}.c" for i in range(STAND_PARTS)]],
    cmd=[WASM2C, "--num-outputs", str(STAND_PARTS), "-n", "prog", "-o", "$(B)/stand/prog.c", wasm2o_wasm.outputs[0]],
    descr="WC",
    color="magenta",
)

stand_parts = [
    command(
        name=f"stand_part{i}",
        deps=[stand_src],
        outputs=[f"$(B)/stand/prog_{i}.o"],
        cmd=[CLANG, *STAND_FLAGS, "-w", "-c", f"$(B)/stand/prog_{i}.c", "-o", f"$(B)/stand/prog_{i}.o"],
        descr="CC",
        color="green",
    )
    for i in range(STAND_PARTS)
]

stand = command(
    name="stand_bin",
    deps=[stand_src, *stand_parts],
    inputs=STAND_HOST,
    outputs=["$(B)/stand/wasm2o"],
    cmd=[CLANG, *STAND_FLAGS, "-I$(S)/dev/stand", "-o", "$(B)/stand/wasm2o", "$(S)/dev/stand/host_w2c.c", "$(S)/dev/stand/wasi.c",
         *STAND_RT, *[p.outputs[0] for p in stand_parts], *build.ldflags],
    descr="LD",
    color="green",
)

# decode release 16 is pure C/C++ wasm; its native column uses the same
# wrappers and the libraries of the ix revision that built that release.
# dev/stand/build.sh supplies those libraries and the host toolchain.
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
        env={"W2X_CHECK": "1"},
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
    parts = 8
    src = command(
        name=f"stand_{kind}_src",
        deps=[wasm],
        outputs=[f"{folder}/mod.h", f"{folder}/mod-impl.h", *[f"{folder}/mod_{i}.c" for i in range(parts)]],
        cmd=[WASM2C, "--num-outputs", str(parts), "-n", "mod", "-o", f"{folder}/mod.c", wasm.outputs[0]],
        descr="WC", color="magenta",
    )
    objects = [command(
        name=f"stand_{kind}_part{i}",
        deps=[src],
        outputs=[f"{folder}/mod_{i}.o"],
        cmd=[CLANG, *STAND_FLAGS, "-I" + folder, "-w", "-c", f"{folder}/mod_{i}.c", "-o", f"{folder}/mod_{i}.o"],
        descr="CC", color="green",
    ) for i in range(parts)]
    reference = command(
        name=f"stand_{kind}_reference",
        inputs=["$(S)/dev/stand/decode.c"],
        deps=[src, *objects],
        outputs=[f"{folder}/reference"],
        cmd=[CLANG, *STAND_FLAGS, "-I" + folder, "-DSTAND_W2C", "-DSTAND_KIND_" + kind.upper(),
             "$(S)/dev/stand/decode.c", *STAND_RT, *[p.outputs[0] for p in objects], "-o", f"{folder}/reference", *build.ldflags, "-lm"],
        descr="LD", color="green",
    )
    decode_stand.extend([obj, native, reference])

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
    "wasm2o": [wasm2o, wasm2o_wasm, wasm2o_o, wasm2o_relinked, wazero, busybox],
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

group("install", busybox, busybox_abi, busybox_o, wasm2o, wasm2o_wasm, wasm2o_o, wazero)
group("test", *tests, wasm2o_go_test, wasm2o_spec_test)
group("stand", stand, wasm2o, wasm2o_wasm, wasm2o_o, busybox, stand_corpus, *decode_stand)
