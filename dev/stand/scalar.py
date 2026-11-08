#!/usr/bin/env python3
"""Apply the stand's scalar native configuration to its private ix checkout.

Each edit starts from the pinned commit, so rerunning is idempotent. The
stand_scalar flag follows target library dependencies, not build tools.
SSE registers remain available for the x86-64 scalar floating-point ABI.
"""
import re
import subprocess
import sys
from pathlib import Path


def main():
    tree = Path(sys.argv[1]).resolve()

    def original(path):
        return subprocess.check_output(["git", "show", "HEAD:pkgs/" + path], cwd=tree, text=True)

    def write(path, text):
        (tree / "pkgs" / path).write_text(text)

    def block(path, name, extra):
        text = original(path)
        pattern = r"({% block " + name + r" %}.*?)(\n{% endblock %})"
        text, n = re.subn(pattern, lambda m: m[1] + "\n{% if stand_scalar %}\n" + extra + "\n{% endif %}" + m[2], text, count=1, flags=re.S)

        if n != 1:
            raise SystemExit(f"{path}: missing block {name}")

        write(path, text)

    # Only the libraries of the workloads and their codec dependencies;
    # libc, the allocator and the host compiler retain their own builds.
    prefixes = ("lib/image/magick", "lib/jpeg", "lib/png", "lib/jxl", "lib/highway", "lib/webp", "lib/tiff", "lib/lcms", "lib/z/", "lib/brotli", "lib/heif", "lib/aom", "lib/gif", "lib/deflate", "lib/pdf/ium", "lib/djvulibre", "lib/svg", "lib/magic")
    condition = " or ".join("name.startswith(" + repr(p) + ")" for p in prefixes)
    flags = "-O3 -march=x86-64-v3 -mno-avx -mno-avx2 -mno-fma -fno-vectorize -fno-slp-vectorize"
    macros = " ".join("-U__" + p + "__" for p in ("SSE", "SSE2", "SSE3", "SSSE3", "SSE4_1", "SSE4_2", "AVX", "AVX2", "FMA"))
    extra = f'''{{% if {condition} %}}
export CFLAGS="${{CFLAGS}} {flags}"
export CXXFLAGS="${{CXXFLAGS}} {flags}"
export CPPFLAGS="${{CPPFLAGS}} -DHWY_COMPILE_ONLY_SCALAR"
{{% if name.startswith('lib/jpeg/open') or name.startswith('lib/pdf/ium') %}}
export CPPFLAGS="${{CPPFLAGS}} {macros}"
{{% endif %}}
{{% endif %}}'''
    block("die/c/ix.sh", "setup_target_flags", extra)
    block("lib/jpeg/turbo/ix.sh", "cmake_flags", "REQUIRE_SIMD=OFF\nWITH_SIMD=OFF")
    block("lib/jxl/t/ix.sh", "cmake_flags", "JPEGXL_ENABLE_AVX512=OFF\nJPEGXL_ENABLE_AVX512_SPR=OFF")
    block("lib/webp/t/ix.sh", "cmake_flags", "WEBP_ENABLE_SIMD=OFF")
    path = "lib/webp/t/ix.sh"
    write(path, (tree / "pkgs" / path).read_text() + '''
{% block patch %}
{% if stand_scalar %}
# libwebp also passes -mno-sse2 for SIMD=OFF; keep scalar double's ABI.
sed -i 's/-mno-sse2//g' cmake/cpu.cmake
{% endif %}
{% endblock %}
''')
    block("lib/png/stock/ix.sh", "configure_flags", "--enable-intel-sse=off")
    block("lib/z/ng/t/ix.sh", "cmake_flags", "WITH_OPTIM=OFF\nWITH_NATIVE_INSTRUCTIONS=OFF")
    # libdeflate's architecture dispatch also enables SIMD with per-function
    # target attributes. Select its portable implementation, as on wasm.
    write("lib/deflate/ix.sh", original("lib/deflate/ix.sh") + '''
{% block patch %}
{% if stand_scalar %}
cat >> common_defs.h << 'EOF'
#undef ARCH_X86_32
#undef ARCH_X86_64
EOF
{% endif %}
{% endblock %}
''')
    block("lib/aom/t/ix.sh", "cmake_flags", "AOM_TARGET_CPU=generic\nCONFIG_RUNTIME_CPU_DETECT=0\nCONFIG_AV1_ENCODER=0\nCONFIG_MULTITHREAD=0\n" + "\n".join("ENABLE_" + x + "=OFF" for x in ("MMX", "SSE", "SSE2", "SSE3", "SSSE3", "SSE4_1", "SSE4_2", "AVX", "AVX2", "AVX512", "NASM")))
    path = "lib/aom/t/ix.sh"
    text = (tree / "pkgs" / path).read_text().replace("{% if x86_64 %}", "{% if x86_64 and not stand_scalar %}")
    text = text.replace("AOM_ARCH_{{target.gnu_arch.upper()}}=1", "{% if not stand_scalar %}\nAOM_ARCH_{{target.gnu_arch.upper()}}=1\n{% endif %}")
    write(path, text)
    # AVIF must use libaom, as the wasm module does, rather than dav1d.
    write("lib/heif/t/ix.sh", original("lib/heif/t/ix.sh").replace("lib/x265\nlib/de265\nlib/dav1d\nlib/svt/av1", "{% if not stand_scalar %}\nlib/x265\nlib/de265\nlib/dav1d\nlib/svt/av1\n{% endif %}"))
    block("lib/heif/common/ix.sh", "cmake_flags", "WITH_DAV1D=OFF\nWITH_LIBDE265=OFF\nWITH_X265=OFF\nWITH_SvtEnc=OFF\nWITH_AOM_DECODER=ON\nWITH_AOM_ENCODER=OFF")
    # svg's kernels are ordinary scalar C++ loops; its SSE4.2 requirement
    # guards the usual vectorized build, which this stand disables.
    write("lib/svg/ix.sh", original("lib/svg/ix.sh") + '''
{% block patch %}
{% if stand_scalar %}
sed -e '/^#if defined(__x86_64__) && !defined(__SSE4_2__)/,/^#endif/d' -i svg.cpp
{% endif %}
{% endblock %}
''')
    text = original("lib/image/magick/t/ix.sh").replace("{% if not wasi %}", "{% if not wasi and not stand_scalar %}")
    text = text.replace("{% block configure_flags %}\n--with-jxl=yes", '''{% block configure_flags %}
--with-jxl=yes
{% if stand_scalar %}
--without-threads
--disable-openmp
--without-raw
--without-openexr
--without-magick-plus-plus
{% endif %}''')
    write("lib/image/magick/t/ix.sh", text)


if __name__ == "__main__":
    main()
