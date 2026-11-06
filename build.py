import build
import shutil
import subprocess
import tomllib
from pathlib import Path


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
RUSTC = tool("rustc")

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

# bin/aot: every crate of bin/aot/crates.toml is one node per target it is
# built for, fetched from crates.io, taken from this repository or from the
# Rust library sources, and compiled by dev/crate.py. The engine restores only
# a node's direct dependencies, so a crate's node depends on its whole
# dependency closure; a target's rlibs share one directory, which is where
# rustc looks for the dependencies of dependencies. Build dependencies are
# always built for the host. wasm32 has no library but the one built here:
# core, compiler_builtins and alloc from rust-src of rustc's own release.
CRATES = tomllib.loads((Path(__file__).parent / "bin/aot/crates.toml").read_text())
SYSROOT = sorted(name for name, spec in CRATES.items() if spec.get("sysroot"))
TARGETS = {"host": None, "wasm32": "wasm32-unknown-unknown"}
RUST = "1.96.1"
LIBRARY = f"rust-src-{RUST}/rust-src/lib/rustlib/src/rust/library"

if f"rustc {RUST} " not in subprocess.run([RUSTC, "--version"], capture_output=True, text=True, check=True).stdout:
    raise RuntimeError(f"{RUSTC} is not rustc {RUST}, the release rust-src is fetched for")

rust_src = fetch(
    "rust_src",
    f"https://static.rust-lang.org/dist/rust-src-{RUST}.tar.xz",
    "b343b6553bc772225f6a2b5be5055017f29794dcf4e08020366b27a0a40fc1c2",
    f"$(B)/ext/rust-src-{RUST}.tar.xz",
)
archives = {}
crates = {}


def rlibs(target):
    return f"$(B)/bin/aot/rlib/{target}"


def rlib(name, target):
    return f"{rlibs(target)}/lib{name.replace('-', '_')}.rlib"


def crate_source(name, spec):
    """The crate's source argument, its inputs and the nodes that fetch it."""
    if "sha256" in spec:
        if name not in archives:
            archives[name] = fetch(
                f"crate_{name}_archive",
                f"https://static.crates.io/crates/{name}/{name}-{spec['version']}.crate",
                spec["sha256"],
                f"$(B)/ext/crates/{name}-{spec['version']}.crate",
            )

        return [archives[name].outputs[0]], [], [archives[name]]

    if "rust-src" in spec:
        return [rust_src.outputs[0], "--subdir", f"{LIBRARY}/{spec['rust-src']}", "--std"], [], [rust_src]

    source = f"$(S)/{spec['path']}"

    return [source], [f"{source}/Cargo.toml", *build.glob(f"{source}/src/**/*.rs")], []


def crate(name, target, output, node):
    spec = CRATES[name]
    library = [] if target == "host" or spec.get("sysroot") else SYSROOT
    closure = set()

    for dep in [*spec.get("deps", []), *library]:
        closure |= {(dep, target)} | crate_node(dep, target)[1]

    for dep in spec.get("build-deps", []):
        closure |= {(dep, "host")} | crate_node(dep, "host")[1]

    source, inputs, fetched = crate_source(name, spec)
    triple = TARGETS[target]
    command_line = [
        "python3", "$(S)/dev/crate.py", RUSTC, *source, output,
        *(["--target", triple] if triple else []),
        *(["--linker", WASM_LD] if output.endswith(".wasm") else []),
        *(arg for feature in spec.get("features", []) for arg in ("--feature", feature)),
        *(arg for dep in spec.get("deps", []) for arg in ("--dep", f"{dep}={rlib(dep, target)}")),
        *(arg for dep in spec.get("build-deps", []) for arg in ("--build-dep", f"{dep}={rlib(dep, 'host')}")),
        "-L", rlibs(target),
        "--build-L", rlibs("host"),
    ]
    target_node = command(
        name=node,
        inputs=[*inputs, "$(S)/dev/crate.py"],
        outputs=[output],
        deps=[*fetched, *(crate_node(*dep)[0] for dep in sorted(closure))],
        cmd=command_line,
        descr="RS",
        color="light-yellow",
    )

    return target_node, closure


def crate_node(name, target):
    if (name, target) not in crates:
        crates[name, target] = crate(name, target, rlib(name, target), f"crate_{name}_{target}")

    return crates[name, target]


aot = crate("aot-cli", "host", "$(B)/bin/aot/aot", "aot")[0]

# aot as a wasm module with no imports: a module's bytes in, the artifact out.
aot_wasm = crate("aot-wasm", "wasm32", "$(B)/bin/aot/aot.wasm", "aot_wasm")[0]

# aot.wasm compiled by aot: what test_aot_wasm.py runs to compile a module again.
aot_self = command(
    name="aot_self",
    outputs=["$(B)/bin/aot/aot.aot"],
    deps=[aot, aot_wasm],
    cmd=[aot.outputs[0], aot_wasm.outputs[0], "$(B)/bin/aot/aot.aot"],
    descr="AO",
    color="light-cyan",
)

# busybox as native code, the way the kernel will load it.
busybox_aot = command(
    name="busybox_aot",
    outputs=["$(B)/bin/busybox/busybox.aot"],
    deps=[aot, busybox],
    cmd=[aot.outputs[0], busybox.outputs[0], "$(B)/bin/busybox/busybox.aot"],
    descr="AO",
    color="light-cyan",
)

# The module test_aot.py runs natively: every operator class, no libc.
aot_ops = command(
    name="aot_ops",
    inputs=["$(S)/tst/aot/ops.c", "$(S)/tst/aot/make.py", "$(S)/dev/wasm.py"],
    outputs=["$(B)/tst/aot/ops.wasm", "$(B)/tst/aot/ops.aot"],
    deps=[aot],
    cmd=[
        ["python3", "$(S)/tst/aot/make.py", CLANG, WASM_LD, "$(B)/tst/aot/ops.wasm"],
        [aot.outputs[0], "$(B)/tst/aot/ops.wasm", "$(B)/tst/aot/ops.aot"],
    ],
    descr="AO",
    color="light-cyan",
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

# What each test runs against: its arguments are the outputs of these nodes.
TESTED = {
    "busybox_abi": [busybox],
    "aot": [aot_ops],
    "aot_wasm": [aot_wasm, aot_self, aot_ops],
}
tests = []

for test in build.glob("$(S)/tst/test_*.py"):
    name = test.rsplit("/", 1)[-1][len("test_"):-len(".py")]
    stamp = f"$(B)/tst/{name}.stamp"
    tested = TESTED[name]
    tests.append(command(
        name=f"test_{name}",
        inputs=[test, "$(S)/dev/wasm_info.py", "$(S)/tst/aot/harness.py"],
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

group("install", busybox, busybox_abi, busybox_aot, aot, aot_wasm)
group("test", *tests)
