import build
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
GO = tool("go")

# Host C (the hosts that run compiled modules) links the host's libc, which
# ix run lib/c bld/compiler passes in CPPFLAGS, CFLAGS and LDFLAGS.
HOST_CFLAGS = [*build.cppflags, *build.cflags]
HOST_LDFLAGS = [*build.ldflags]

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

# bin/aot: the compiler, a Go program with no dependencies, built natively
# and as a WASI module.
AOT_SOURCES = [*build.glob("$(S)/bin/aot/*.go"), "$(S)/bin/aot/go.mod", "$(S)/dev/go.py"]

aot = command(
    name="aot",
    inputs=AOT_SOURCES,
    outputs=["$(B)/bin/aot/aot"],
    cmd=["python3", "$(S)/dev/go.py", "build", GO, "$(S)/bin/aot", "$(B)/bin/aot/aot"],
    descr="GO",
    color="light-blue",
)

aot_wasm = command(
    name="aot_wasm",
    inputs=AOT_SOURCES,
    outputs=["$(B)/bin/aot/aot.wasm"],
    cmd=["python3", "$(S)/dev/go.py", "build", GO, "$(S)/bin/aot", "$(B)/bin/aot/aot.wasm", "--wasip1"],
    descr="GO",
    color="light-blue",
)

# aot.wasm compiled by aot, and linked with the WASI host of bin/aot/rt: the
# compiler as its own output.
aot_self_obj = command(
    name="aot_self_obj",
    outputs=["$(B)/bin/aot/self/prog.o", "$(B)/bin/aot/self/prog.h"],
    deps=[aot, aot_wasm],
    cmd=[aot.outputs[0], "-p", "prog", "-H", "$(B)/bin/aot/self/prog.h", "-o", "$(B)/bin/aot/self/prog.o", aot_wasm.outputs[0]],
    descr="AO",
    color="light-cyan",
)

aot_self = command(
    name="aot_self",
    inputs=build.glob("$(S)/bin/aot/rt/*"),
    outputs=["$(B)/bin/aot/aot.self"],
    deps=[aot_self_obj],
    cmd=[
        CLANG, *HOST_CFLAGS, "-O2", "-I$(S)/bin/aot/rt", "-I$(B)/bin/aot/self", "-o", "$(B)/bin/aot/aot.self",
        "$(S)/bin/aot/rt/host.c", "$(S)/bin/aot/rt/wasi.c", aot_self_obj.outputs[0], *HOST_LDFLAGS,
    ],
    descr="LD",
    color="green",
)

# tst/wazero: a WASI runner on wazero, what aot is measured against. Its
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

# What each test runs against: its arguments are the outputs of these nodes.
TESTED = {
    "busybox_abi": [busybox],
    "aot": [aot, aot_wasm, aot_self_obj, aot_self, wazero, busybox],
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

group("install", busybox, busybox_abi, aot, aot_wasm, aot_self, wazero)
group("test", *tests)
