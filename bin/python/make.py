#!/usr/bin/env python3
"""Build CPython for pizdos from a release tarball, our pyconfig.h and lib/c.

    make.py core <cc> <ar> <wasm-ld> <tarball> <pyconfig.h> <libc-include> <libc.a> <libpython.a> <freeze.wasm>
    make.py python <cc> <wasm-ld> <vm> <tarball> <pyconfig.h> <libc-include> <libc.a> <libpython.a> <freeze.wasm> <python.wasm> <python314.zip>

core compiles the interpreter and its built-in modules into libpython.a,
the objects the tarball's Makefile.pre.in names and the modules below, and
links Programs/_freeze_module against it. python compiles that freezer
with vm and runs it in the kernel to freeze the modules the Makefile
freezes, links python.wasm with them, and stores the standard library,
without its tests, in a zip with the build's sysconfig data. Threads are
CPython's stubs, there is no dynamic loading, every module is built in and
none needs more than lib/c: no sockets, no compression. An undefined symbol
is a link error, as for every program of lib/c.
"""
import calendar
import os
import pprint
import re
import shutil
import subprocess
import sys
import tarfile
import time
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent.parent / "dev"))

import wasm  # noqa: E402

VERSION = "3.14"
PREFIX = "/usr"
PLATFORM = "pizdos"

FLAGS = [
    "-O3",
    "-std=c11",
    "-fwrapv",
    "-fno-strict-overflow",
    "-DNDEBUG",
    "-w",
    "-Werror=implicit-function-declaration",
    "-Werror=int-conversion",
    "-Werror=incompatible-pointer-types",
]

LINK = [
    "--stack-first",
    "-z", "stack-size=8389632",
    "--max-memory=4294967296",
    "--export=__heap_base",
    "--export=__stack_pointer",
    "--gc-sections",
    "--error-limit=0",
]

# The built-in modules beyond Modules/Setup.bootstrap.in's: those of the
# standard library that need nothing but libc, by name and sources.
MODULES = {
    "array": ["arraymodule.c"],
    "_asyncio": ["_asynciomodule.c"],
    "_bisect": ["_bisectmodule.c"],
    "_csv": ["_csv.c"],
    "_heapq": ["_heapqmodule.c"],
    "_json": ["_json.c"],
    "_lsprof": ["_lsprof.c", "rotatingtree.c"],
    "_pickle": ["_pickle.c"],
    "_queue": ["_queuemodule.c"],
    "_random": ["_randommodule.c"],
    "_struct": ["_struct.c"],
    "_zoneinfo": ["_zoneinfo.c"],
    "math": ["mathmodule.c"],
    "cmath": ["cmathmodule.c"],
    "_statistics": ["_statisticsmodule.c"],
    "binascii": ["binascii.c"],
    "unicodedata": ["unicodedata.c"],
    "fcntl": ["fcntlmodule.c"],
    "grp": ["grpmodule.c"],
    "mmap": ["mmapmodule.c"],
    "_posixsubprocess": ["_posixsubprocess.c"],
    "resource": ["resource.c"],
    "select": ["selectmodule.c"],
    "syslog": ["syslogmodule.c"],
    "termios": ["termios.c"],
    "_md5": ["md5module.c"],
    "_sha1": ["sha1module.c"],
    "_sha2": ["sha2module.c"],
    "_sha3": ["sha3module.c"],
    "_blake2": ["blake2module.c"],
    "_hmac": ["hmacmodule.c"],
}

# HACL*, the hashes' implementation the tarball carries, without its SIMD
# variants.
HACL = [
    "Hacl_Hash_MD5.c",
    "Hacl_Hash_SHA1.c",
    "Hacl_Hash_SHA2.c",
    "Hacl_Hash_SHA3.c",
    "Hacl_Hash_Blake2b.c",
    "Hacl_Hash_Blake2s.c",
    "Hacl_HMAC.c",
    "Hacl_Streaming_HMAC.c",
    "Lib_Memzero0.c",
]
HACL_FLAGS = ["-I", "Modules/_hacl", "-I", "Modules/_hacl/include", "-D_BSD_SOURCE", "-D_DEFAULT_SOURCE"]

# The standard library's packages the zip leaves out: tests, the GUI and
# pip's bundled wheels.
LIB_EXCLUDED = {"test", "idlelib", "tkinter", "turtledemo", "ensurepip", "site-packages"}

# The objects of Makefile.pre.in's variables, and those its configure
# substitutions name: no dynamic loading.
MAKEFILE_OBJECTS = ["PARSER_OBJS", "OBJECT_OBJS", "PYTHON_OBJS", "MODULE_OBJS"]
SUBSTITUTED = ["Python/dynload_stub.o", "Modules/getbuildinfo.o"]
# Objects of the programs, not of the library.
PROGRAM_OBJECTS = {"Python/frozen.o", "Modules/getpath.o", "Modules/config.o"}


def extract(tarball, work):
    with tarfile.open(tarball) as archive:
        archive.extractall(work, filter="data")
        newest = max(member.mtime for member in archive.getmembers())

    return next(work.glob("Python-*")), newest


def makefile_variables(tree):
    """Makefile.pre.in's variable assignments, continuation lines joined."""
    text = (tree / "Makefile.pre.in").read_text().replace("\\\n", " ")
    values = {}

    for line in text.splitlines():
        found = re.match(r"^([A-Z_][A-Z0-9_]*)\s*=(.*)$", line)

        if found:
            values[found.group(1)] = found.group(2)

    return values


def core_objects(tree):
    """The library's objects: the Makefile's, its variables expanded."""
    values = makefile_variables(tree)

    def expand(name, seen=()):
        out = []

        for word in values.get(name, "").split():
            ref = re.fullmatch(r"\$\((\w+)\)", word)

            if ref and ref.group(1) not in seen:
                out += expand(ref.group(1), seen + (name,))
            elif word.endswith(".o") and "$" not in word and "@" not in word:
                out.append(word)

        return out

    objects = []

    for name in MAKEFILE_OBJECTS:
        objects += expand(name)

    objects += SUBSTITUTED

    return [o for o in dict.fromkeys(objects) if o not in PROGRAM_OBJECTS]


def bootstrap_modules(tree):
    """Modules/Setup.bootstrap.in's modules by name and sources."""
    modules = {}

    for line in (tree / "Modules" / "Setup.bootstrap.in").read_text().splitlines():
        line = re.sub(r"@\w+@", "", line).strip()

        if line and not line.startswith(("#", "*")):
            name, *sources = line.split()
            modules[name] = sources

    return modules


def frozen_modules(tree):
    """The modules Makefile.pre.in freezes: name, source, header name."""
    out = []

    for line in (tree / "Makefile.pre.in").read_text().splitlines():
        found = re.match(r"^\t\$\(FREEZE_MODULE\w*\) (\S+) \$\(srcdir\)/(\S+) Python/frozen_modules/(\S+)$", line)

        if found:
            out.append(found.groups())

    if not out:
        raise SystemExit("no frozen modules in Makefile.pre.in")

    return out


def config_c(tree, modules):
    names = list(modules)
    text = (tree / "Modules" / "config.c.in").read_text()
    text = text.replace("/* -- ADDMODULE MARKER 1 -- */", "\n".join(f"extern PyObject* PyInit_{n}(void);" for n in names))
    text = text.replace("/* -- ADDMODULE MARKER 2 -- */", "\n".join(f'    {{"{n}", PyInit_{n}}},' for n in names))

    return text


def object_name(work, source):
    """A unique object path for a source path of the tree."""
    return work / "obj" / (str(Path(source).with_suffix("")).replace("/", "_") + ".o")


def base_flags(tree, include, libc_include):
    return FLAGS + [
        "-isystem", str(Path(libc_include).resolve()),
        "-I", str(include),
        "-I", str(tree / "Include"),
        "-I", str(tree / "Include" / "internal"),
    ]


def build_date(newest):
    """The tarball's newest file's time, as __DATE__ and __TIME__ write it."""
    t = time.gmtime(newest)

    return f"{calendar.month_abbr[t.tm_mon]} {t.tm_mday:2d} {t.tm_year}", time.strftime("%H:%M:%S", t)


def core(cc, ar, ld, tarball, pyconfig, libc_include, libc, libpython, freeze):
    work = Path(os.environ.get("TMPDIR") or "/tmp") / "python-core"
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    tree, newest = extract(tarball, work)
    include = work / "include"
    include.mkdir()
    shutil.copy(pyconfig, include / "pyconfig.h")
    flags = base_flags(tree, include, libc_include)
    date, clock = build_date(newest)
    special = {
        "Python/getplatform.o": [f'-DPLATFORM="{PLATFORM}"'],
        "Python/sysmodule.o": ['-DABIFLAGS=""'],
        "Modules/getbuildinfo.o": [f'-DDATE="{date}"', f'-DTIME="{clock}"'],
    }
    units = []

    for obj in core_objects(tree):
        source = obj[:-2] + ".c"
        units.append((tree / source, object_name(work, source), flags + ["-DPy_BUILD_CORE"] + special.get(obj, [])))

    modules = {**bootstrap_modules(tree), **MODULES}

    for name, sources in modules.items():
        for source in sources:
            extra = HACL_FLAGS if name in ("_md5", "_sha1", "_sha2", "_sha3", "_blake2", "_hmac") else []
            units.append((tree / "Modules" / source, object_name(work, "Modules/" + source), flags + extra + ["-DPy_BUILD_CORE_BUILTIN"]))

    for source in HACL:
        units.append((tree / "Modules" / "_hacl" / source, object_name(work, "Modules/_hacl/" + source), flags + HACL_FLAGS))

    config = work / "config.c"
    config.write_text(config_c(tree, modules))
    units.append((config, object_name(work, "Modules/config.c"), flags + ["-DPy_BUILD_CORE"]))
    programs = [
        (tree / "Programs" / "_freeze_module.c", object_name(work, "Programs/_freeze_module.c"), flags + ["-DPy_BUILD_CORE"]),
        (tree / "Modules" / "getpath_noop.c", object_name(work, "Modules/getpath_noop.c"), flags + ["-DPy_BUILD_CORE"]),
    ]
    wasm.compile_all(cc, units + programs, cwd=tree)
    wasm.archive(ar, libpython, [obj for _, obj, _ in units])
    Path(freeze).parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([ld, *LINK, "-o", freeze, *(str(obj) for _, obj, _ in programs), libpython, libc], check=True)


def sysconfigdata(pyconfig):
    """The sysconfig module's build_time_vars: pyconfig.h's definitions and
    the Makefile's values sysconfig and its users read."""
    values = {}

    for found in re.finditer(r"^#define (\w+)(?:[ \t]+(.*))?$", Path(pyconfig).read_text(), re.M):
        name, value = found.group(1), (found.group(2) or "").strip()

        if name == "Py_PYCONFIG_H":
            continue

        if value == "":
            values[name] = 1
        elif re.fullmatch(r"-?\d+", value):
            values[name] = int(value)
        else:
            values[name] = value.strip('"')

    values.update({
        "ABIFLAGS": "",
        "MACHDEP": PLATFORM,
        "MULTIARCH": "",
        "HOST_GNU_TYPE": f"wasm32-unknown-{PLATFORM}",
        "VERSION": VERSION,
        "prefix": PREFIX,
        "exec_prefix": PREFIX,
        "BINDIR": f"{PREFIX}/bin",
        "LIBDIR": f"{PREFIX}/lib",
        "LIBDEST": f"{PREFIX}/lib/python{VERSION}",
        "BINLIBDEST": f"{PREFIX}/lib/python{VERSION}",
        "INCLUDEPY": f"{PREFIX}/include/python{VERSION}",
        "CC": "clang --target=wasm32-unknown-unknown",
        "CFLAGS": " ".join(FLAGS),
        "PY_CFLAGS": " ".join(FLAGS),
        "PY_CFLAGS_NODIST": "",
        "PY_CORE_CFLAGS": " ".join(FLAGS + ["-DPy_BUILD_CORE"]),
        "CONFIG_ARGS": "",
        "LDFLAGS": "",
        "PY_LDFLAGS": "",
        "CCSHARED": "",
        "LDSHARED": "",
        "SOABI": f"cpython-{VERSION.replace('.', '')}-{PLATFORM}",
        "EXT_SUFFIX": f".cpython-{VERSION.replace('.', '')}-{PLATFORM}.so",
        "SHLIB_SUFFIX": ".so",
        "Py_DEBUG": 0,
        "Py_GIL_DISABLED": 0,
        "Py_ENABLE_SHARED": 0,
        "LIBRARY": f"libpython{VERSION}.a",
        "LDLIBRARY": f"libpython{VERSION}.a",
        "TZPATH": "/usr/share/zoneinfo",
    })

    return "# The build of CPython for pizdos.\nbuild_time_vars = " + pprint.pformat(values, width=100) + "\n"


def stdlib_zip(tree, pyconfig, output):
    """Lib without LIB_EXCLUDED, stored (zipimport needs zlib to inflate),
    its entries sorted and dated 1980."""
    files = {}

    for path in sorted((tree / "Lib").rglob("*")):
        rel = path.relative_to(tree / "Lib")

        if path.is_file() and rel.parts[0] not in LIB_EXCLUDED and "__pycache__" not in rel.parts:
            files[str(rel)] = path.read_bytes()

    files[f"_sysconfigdata__{PLATFORM}_.py"] = sysconfigdata(pyconfig).encode()
    Path(output).parent.mkdir(parents=True, exist_ok=True)

    with zipfile.ZipFile(output, "w", zipfile.ZIP_STORED) as z:
        for name in sorted(files):
            info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            info.external_attr = 0o644 << 16
            z.writestr(info, files[name])


def python(cc, ld, vm, tarball, pyconfig, libc_include, libc, libpython, freeze, output, zipped):
    work = Path(os.environ.get("TMPDIR") or "/tmp") / "python"
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    tree, _ = extract(tarball, work)
    include = work / "include"
    include.mkdir()
    shutil.copy(pyconfig, include / "pyconfig.h")
    freezer = work / "freeze.o"
    subprocess.run([vm, "-o", str(freezer), freeze], check=True)

    # The headers go where frozen.c and getpath.c include them from: the
    # first as frozen_modules/, the second as Python/frozen_modules/.
    frozen = work / "gen" / "frozen_modules"
    frozen.mkdir(parents=True)
    out = tree / "frozen"
    out.mkdir()

    for name, source, header in frozen_modules(tree):
        subprocess.run([vm, "run", "--root", str(tree), "-a", "_freeze_module", "--", str(freezer),
                        name, "/" + source, "/frozen/" + header], check=True, env={"PATH": "/bin"})
        shutil.copy(out / header, frozen / header)

    (work / "gen" / "Python").mkdir()
    shutil.copytree(frozen, work / "gen" / "Python" / "frozen_modules")
    flags = base_flags(tree, include, libc_include) + ["-I", str(work / "gen"), "-DPy_BUILD_CORE"]
    getpath = [f'-DPYTHONPATH=""', f'-DPREFIX="{PREFIX}"', f'-DEXEC_PREFIX="{PREFIX}"', f'-DVERSION="{VERSION}"',
               '-DVPATH=""', '-DPLATLIBDIR="lib"', '-DPYTHONFRAMEWORK=""']
    units = [
        (tree / "Programs" / "python.c", object_name(work, "Programs/python.c"), flags),
        (tree / "Python" / "frozen.c", object_name(work, "Python/frozen.c"), flags),
        (tree / "Modules" / "getpath.c", object_name(work, "Modules/getpath.c"), flags + getpath),
    ]
    wasm.compile_all(cc, units, cwd=tree)
    Path(output).parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([ld, *LINK, "-o", output, *(str(obj) for _, obj, _ in units), libpython, libc], check=True)
    stdlib_zip(tree, pyconfig, zipped)


def main():
    mode, args = sys.argv[1], sys.argv[2:]

    if mode == "core":
        core(*args)
    elif mode == "python":
        python(*args)
    else:
        raise SystemExit(f"unknown mode {mode}")


if __name__ == "__main__":
    main()
