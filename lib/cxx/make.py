#!/usr/bin/env python3
"""Build lib/cxx: libc++ and libc++abi of the LLVM release tarball, for lib/c.

    make.py <cc> <ar> <llvm-tarball> <libc-include> <libc++.a> <include.tar>

The flags and sources are those of LLVM's runtimes build configured for this
system (__config_site: lib/c's threads, locales and wide characters, and
the filesystem library), without its probes: libc++abi without exceptions,
new and delete from libc++abi, thread_local destructors registered with
lib/c.
libc++.a holds both libraries; include.tar is include/c++/v1, the headers a
C++ program includes before lib/c's.
"""
import os
import shutil
import sys
import tarfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT.parent.parent / "dev"))

import wasm  # noqa: E402

PARTS = ["libcxx/include/", "libcxx/src/", "libcxx/vendor/", "libcxxabi/include/", "libcxxabi/src/", "libunwind/include/",
         "libc/"]

CXX_SOURCES = """
algorithm.cpp any.cpp atomic.cpp barrier.cpp bind.cpp call_once.cpp charconv.cpp chrono.cpp
condition_variable.cpp condition_variable_destructor.cpp error_category.cpp exception.cpp expected.cpp
filesystem/directory_entry.cpp filesystem/directory_iterator.cpp filesystem/filesystem_clock.cpp
filesystem/filesystem_error.cpp filesystem/int128_builtins.cpp filesystem/operations.cpp filesystem/path.cpp fstream.cpp
functional.cpp future.cpp hash.cpp ios.cpp ios.instantiations.cpp iostream.cpp locale.cpp memory.cpp
memory_resource.cpp mutex.cpp mutex_destructor.cpp new_handler.cpp new_helpers.cpp optional.cpp ostream.cpp
print.cpp random.cpp random_shuffle.cpp regex.cpp ryu/d2fixed.cpp ryu/d2s.cpp ryu/f2s.cpp shared_mutex.cpp
stdexcept.cpp string.cpp strstream.cpp system_error.cpp text_encoding.cpp thread.cpp typeinfo.cpp valarray.cpp
variant.cpp vector.cpp verbose_abort.cpp
""".split()

CXXABI_SOURCES = """
abort_message.cpp cxa_aux_runtime.cpp cxa_default_handlers.cpp cxa_demangle.cpp cxa_exception_storage.cpp
cxa_guard.cpp cxa_handlers.cpp cxa_noexception.cpp cxa_vector.cpp cxa_virtual.cpp
fallback_malloc.cpp private_typeinfo.cpp stdlib_exception.cpp stdlib_new_delete.cpp stdlib_stdexcept.cpp
stdlib_typeinfo.cpp
""".split()

COMMON = ["-O2", "-nostdinc++", "-fno-exceptions", "-w", "-DLIBCXX_BUILDING_LIBCXXABI", "-D_FILE_OFFSET_BITS=64",
          "-D_LARGEFILE_SOURCE", "-D_LIBCPP_AVAILABILITY_MINIMUM_HEADER_VERSION=2", "-D_LIBCPP_BUILDING_LIBRARY",
          "-D_LIBCPP_HAS_NO_PRAGMA_SYSTEM_HEADER", "-D__STDC_CONSTANT_MACROS", "-D__STDC_FORMAT_MACROS",
          "-D__STDC_LIMIT_MACROS", "-fvisibility-inlines-hidden", "-fvisibility=hidden", "-fsized-deallocation"]
# libc++'s floating-point from_chars is LLVM libc's, in its own namespace
CXX_FLAGS = ["-std=c++26", "-D_LIBCPP_REMOVE_TRANSITIVE_INCLUDES", "-DLIBC_NAMESPACE=__llvm_libc_common_utils",
             "-faligned-allocation"]
CXXABI_FLAGS = ["-std=c++23", "-D_LIBCPP_HAS_THREAD_API_PTHREAD", "-D_LIBCXXABI_BUILDING_LIBRARY", "-D_DEBUG",
                "-fstrict-aliasing"]


def extract(tarball, work):
    with tarfile.open(tarball) as archive:
        top = None

        for member in archive:
            name = member.name.split("/", 1)

            if len(name) < 2:
                continue

            top = name[0]

            if any(name[1].startswith(part) for part in PARTS):
                archive.extract(member, work, filter="data")

    return work / top


def headers(tree, include):
    shutil.copytree(tree / "libcxx/include", include, ignore=shutil.ignore_patterns("CMakeLists.txt", "*.in"))
    shutil.copy(ROOT / "__config_site", include / "__config_site")
    # the unstable ABI but for the alternate string layout, as ix's libc++
    abi = include / "__configuration/abi.h"
    text = abi.read_text()
    layout = "#  define _LIBCPP_ABI_ALTERNATE_STRING_LAYOUT\n"

    if text.count(layout) != 1:
        raise SystemExit("abi.h does not define _LIBCPP_ABI_ALTERNATE_STRING_LAYOUT once")

    abi.write_text(text.replace(layout, ""))
    shutil.copy(tree / "libcxx/vendor/llvm/default_assertion_handler.in", include / "__assertion_handler")

    for name in ("cxxabi.h", "__cxxabi_config.h"):
        shutil.copy(tree / "libcxxabi/include" / name, include / name)


def main():
    cc, ar, tarball, libc_include, library, include_tar = sys.argv[1:7]
    work = Path(os.environ.get("TMPDIR") or "/tmp") / "cxx"
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    tree = extract(tarball, work)
    include = work / "out" / "include" / "c++" / "v1"
    headers(tree, include)
    paths = ["-I", str(tree / "libcxx/src"), "-I", str(include), "-I", str(tree / "libcxxabi/include"),
             "-isystem", str(tree / "libc"), "-isystem", str(Path(libc_include).resolve()),
             "-isystem", str(tree / "libunwind/include")]
    units = []

    for source in CXX_SOURCES:
        obj = work / "obj" / ("cxx_" + source.replace("/", "_")).replace(".cpp", ".o")
        units.append((tree / "libcxx/src" / source, obj, COMMON + CXX_FLAGS + paths))

    for source in CXXABI_SOURCES:
        obj = work / "obj" / ("cxxabi_" + source).replace(".cpp", ".o")
        units.append((tree / "libcxxabi/src" / source, obj, COMMON + CXXABI_FLAGS + paths))

    wasm.compile_all(cc, units)
    wasm.archive(ar, library, [obj for source, obj, flags in units])
    wasm.write_tar(include_tar, work / "out", include)


if __name__ == "__main__":
    main()
