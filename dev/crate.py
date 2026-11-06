#!/usr/bin/env python3
"""Compile one Rust crate with rustc; cargo is not involved.

    crate.py <rustc> <source> <output> [--target T] [--subdir D] [--std] [--linker L]
             [--feature F]... [--dep NAME=RLIB]... [--build-dep NAME=RLIB]... [-L DIR]... [--build-L DIR]

<source> is a .crate archive fetched from crates.io, a tarball whose
directory D is the crate (the Rust library sources), or a crate directory of
this repository. The crate's Cargo.toml gives what is the crate's own: its
edition, its target path, the extern names of renamed dependencies and its
build script. The feature set and the dependency set are the graph's. NAME is
a dependency's package name; -L is where rustc finds the dependencies of
dependencies and, for a target without a prebuilt library, core and alloc;
--build-L is the same for the build script, which is built for the host.
An output ending in .rlib is the library, in .wasm a cdylib linked by L,
anything else the crate's binary. --std builds a crate of the Rust library
itself.

A build script is compiled for the host against the build-dependencies, run
in the crate directory the way cargo runs it, and its cargo: directives
become rustc flags; a directive this driver does not know fails the build.
"""
import os
import subprocess
import sys
import tarfile
import tomllib
from pathlib import Path

FLAGS = ["-C", "opt-level=3", "-C", "debuginfo=0", "-C", "embed-bitcode=no"]
LISTS = ("feature", "dep", "build-dep", "L")
VALUES = ("target", "subdir", "linker", "build-L")


def parse(argv):
    options = {name: [] for name in LISTS}
    options.update({name: None for name in VALUES})
    options["std"] = False
    rest = []
    args = iter(argv)

    for arg in args:
        name = arg.lstrip("-")

        if name in LISTS:
            options[name].append(next(args))
        elif name in VALUES:
            options[name] = next(args)
        elif arg == "--std":
            options["std"] = True
        else:
            rest.append(arg)

    return rest, options


def unpack(source, subdir, work):
    """The crate directory, and whether it came from an archive."""
    if Path(source).is_dir():
        return Path(source).resolve(), False

    # The whole archive: the library's crates refer to their siblings by path.
    with tarfile.open(source) as archive:
        archive.extractall(work, filter="data")

    if subdir:
        return work / subdir, True

    return work / Path(source).name.removesuffix(".crate"), True


def externs(manifest, deps):
    """--extern flags for deps, named the way the crate's code refers to them."""
    tables = [manifest]
    tables.extend(manifest.get("target", {}).values())
    keys = {}

    for table in tables:
        for section in ("dependencies", "build-dependencies"):
            for key, spec in table.get(section, {}).items():
                package = spec.get("package", key) if isinstance(spec, dict) else key
                keys.setdefault(package, key)

    flags = []

    for dep in deps:
        package, rlib = dep.split("=", 1)

        if package not in keys:
            raise SystemExit(f"{manifest['package']['name']} does not depend on {package}")

        flags += ["--extern", f"{keys[package].replace('-', '_')}={rlib}"]

    return flags


def package_env(manifest, root):
    package = manifest["package"]
    version = package["version"]
    core, _, pre = version.partition("-")
    major, minor, patch = core.split(".")

    return {
        "CARGO_PKG_NAME": package["name"],
        "CARGO_CRATE_NAME": package["name"].replace("-", "_"),
        "CARGO_PKG_VERSION": version,
        "CARGO_PKG_VERSION_MAJOR": major,
        "CARGO_PKG_VERSION_MINOR": minor,
        "CARGO_PKG_VERSION_PATCH": patch,
        "CARGO_PKG_VERSION_PRE": pre,
        "CARGO_PKG_AUTHORS": ":".join(package.get("authors", [])),
        "CARGO_PKG_DESCRIPTION": package.get("description", ""),
        "CARGO_PKG_REPOSITORY": package.get("repository", ""),
        "CARGO_PKG_LICENSE": package.get("license", ""),
        "CARGO_MANIFEST_DIR": str(root),
        "CARGO_MANIFEST_PATH": str(root / "Cargo.toml"),
    }


def target_cfg(rustc, target):
    """CARGO_CFG_* of the target the crate is built for."""
    command = [rustc, "--print", "cfg", "--target", target]
    lines = subprocess.run(command, check=True, capture_output=True, text=True).stdout.splitlines()
    values = {}

    for line in lines:
        name, _, value = line.partition("=")
        values.setdefault(name, []).append(value.strip('"'))

    return {f"CARGO_CFG_{name.upper()}": ",".join(v for v in value if v) for name, value in values.items()}


def host_triple(rustc):
    for line in subprocess.run([rustc, "-vV"], check=True, capture_output=True, text=True).stdout.splitlines():
        if line.startswith("host: "):
            return line.removeprefix("host: ")

    raise SystemExit(f"{rustc} -vV names no host")


def run_build_script(rustc, root, manifest, script, features, options, work, env):
    """Compile and run the build script; return its rustc flags and environment."""
    package = manifest["package"]
    binary = work / "build-script"
    out_dir = work / "out"
    out_dir.mkdir()
    host = host_triple(rustc)
    target = options["target"] or host
    command = [
        rustc, "--crate-name", "build_script_build", "--edition", package.get("edition", "2015"),
        "--crate-type", "bin", str(root / script), "-o", str(binary), "--cap-lints", "allow",
        *FLAGS,
        *(f"--cfg=feature=\"{feature}\"" for feature in features),
        *externs(manifest, options["build-dep"]),
        *([f"-L{options['build-L']}"] if options["build-L"] else []),
    ]
    subprocess.run(command, check=True, env=env)
    script_env = dict(env)
    script_env.update(target_cfg(rustc, target))
    script_env.update({
        "OUT_DIR": str(out_dir),
        "TARGET": target,
        "HOST": host,
        "RUSTC": rustc,
        "OPT_LEVEL": "3",
        "PROFILE": "release",
        "DEBUG": "false",
        "NUM_JOBS": os.environ.get("BUILD_JOBS", "1"),
        "CARGO_ENCODED_RUSTFLAGS": "",
        # cranelift-codegen's script stamps its version with `git rev-parse
        # HEAD` of whatever repository encloses the crate; there is none.
        "GIT_DIR": str(work / "no-git"),
    })

    for feature in features:
        script_env["CARGO_FEATURE_" + feature.upper().replace("-", "_")] = "1"

    result = subprocess.run([str(binary)], cwd=root, env=script_env, capture_output=True, text=True)

    if result.returncode != 0:
        sys.stderr.write(result.stdout + result.stderr)
        raise SystemExit(f"build script of {package['name']} exited {result.returncode}")

    flags = []
    rustc_env = {"OUT_DIR": str(out_dir)}

    for line in result.stdout.splitlines():
        if not line.startswith("cargo:"):
            continue

        directive, _, value = line.removeprefix("cargo:").lstrip(":").partition("=")

        if directive == "rustc-cfg":
            flags += ["--cfg", value]
        elif directive == "rustc-env":
            name, _, setting = value.partition("=")
            rustc_env[name] = setting
        elif directive in ("rustc-check-cfg", "rerun-if-changed", "rerun-if-env-changed", "metadata"):
            pass
        elif directive == "warning":
            sys.stderr.write(f"{package['name']}: {value}\n")
        elif "links" in package and not directive.startswith("rustc-"):
            # cargo:KEY=VALUE of a `links` crate is metadata for its dependents; none reads it.
            pass
        else:
            raise SystemExit(f"build script of {package['name']}: unsupported directive {line!r}")

    return flags, rustc_env


def main():
    (rustc, source, output), options = parse(sys.argv[1:])
    work = Path(os.environ.get("TMPDIR") or "/tmp") / "crate"
    work.mkdir(parents=True, exist_ok=True)
    root, fetched = unpack(source, options["subdir"], work)
    manifest = tomllib.loads((root / "Cargo.toml").read_text())
    package = manifest["package"]
    features = sorted(options["feature"])
    env = dict(os.environ)
    env.update(package_env(manifest, root))
    flags = []
    script = package.get("build", "build.rs" if (root / "build.rs").is_file() else False)

    if options["std"]:
        env["RUSTC_BOOTSTRAP"] = "1"
        flags += ["-Z", "force-unstable-if-unmarked"]

    if script:
        script_flags, script_env = run_build_script(rustc, root, manifest, script, features, options, work, env)
        flags += script_flags
        env.update(script_env)

    lib = manifest.get("lib", {})

    if output.endswith(".rlib") or output.endswith(".wasm"):
        name = lib.get("name", package["name"].replace("-", "_"))
        kind = ["--crate-type", "lib" if output.endswith(".rlib") else "cdylib", root / lib.get("path", "src/lib.rs")]
    else:
        binary = manifest.get("bin", [{}])[0]
        name = binary.get("name", package["name"]).replace("-", "_")
        kind = ["--crate-type", "bin", root / binary.get("path", "src/main.rs")]

    if options["target"]:
        flags += ["--target", options["target"]]

    if options["linker"]:
        flags += ["-C", f"linker={options['linker']}", "-C", "linker-flavor=wasm-ld"]

    command = [
        rustc, "--crate-name", name, "--edition", package.get("edition", "2015"), *kind, "-o", output,
        *FLAGS,
        "-C", f"metadata={package['name']}-{package['version']}",
        f"--remap-path-prefix={root}={package['name']}-{package['version']}",
        *(f"--cfg=feature=\"{feature}\"" for feature in features),
        *externs(manifest, options["dep"]),
        *(f"-L{path}" for path in options["L"]),
        *flags,
    ]

    if fetched:
        command += ["--cap-lints", "allow"]
    else:
        command += ["-D", "warnings"]

    Path(output).parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([str(x) for x in command], check=True, env=env)


if __name__ == "__main__":
    main()
