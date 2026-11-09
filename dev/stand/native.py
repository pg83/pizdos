#!/usr/bin/env python3
"""Build decode's pinned wasm wrapper against the native libraries from ix.

    native.py KIND SOURCE OUTPUT CONFIG_JSON

The wrapper's pointers are native pointers. Native static constructors have
already run, and its explicit wasm constructor call becomes a no-op in the
driver. Wrapper traps leave the current file through the driver's landing.
"""
import json
import re
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path


def main():
    kind, source, output, config = sys.argv[1:]
    config = json.loads(config)
    here = Path(__file__).resolve().parent
    output = Path(output).resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    common = [*config["cppflags"], *config["cflags"], "-O3", "-march=x86-64-v3", "-mno-avx", "-mno-avx2", "-mno-fma", "-fno-vectorize", "-fno-slp-vectorize", "-fomit-frame-pointer", "-flto=thin"]

    with tempfile.TemporaryDirectory(prefix="native-", dir=output.parent) as tmp:
        tmp = Path(tmp)
        text = Path(source).read_text()
        text = re.sub(r'__attribute__\(\(export_name\("[^"]+"\)\)\)', "", text)
        support = """
#ifdef __cplusplus
extern "C" {
#endif
void stand_trap(void) __attribute__((noreturn));
extern char g_exception_cause[256];
#ifdef __cplusplus
}
#endif
#define __builtin_trap() stand_trap()
"""
        wrapper = tmp / ("wrapper.cpp" if kind == "djvu" else "wrapper.c")
        wrapper.write_text(support + text)
        extra = []

        if kind == "im":
            extra = shlex.split(subprocess.check_output([config["pkg_config"], "--cflags", "MagickWand-7.Q16HDRI"], text=True))

        cxx = kind == "djvu"
        flags = [*config["cxxflags"], "-DHAVE_CONFIG_H", "-DHAVE_PTHREAD=1"] if cxx else []
        subprocess.run([config["cxx"] if cxx else config["cc"], *common, *flags, *extra, "-c", str(wrapper), "-o", str(tmp / "wrapper.o")], check=True)
        subprocess.run([config["cc"], *common, "-DSTAND_KIND_" + kind.upper(), "-I" + str(here),
                        "-c", str(here / "decode.c"), "-o", str(tmp / "driver.o")], check=True)
        objects = [str(tmp / "driver.o"), str(tmp / "wrapper.o")]

        if kind == "magic":
            data = Path(config["magic_data"])

            if not data.is_file():
                raise SystemExit("MAGIC_DATA is missing: build the stand through dev/stand/build.sh")

            asm = tmp / "magic.s"
            asm.write_text(f'.section .rodata\n.balign 16\n.globl magic_mgc\nmagic_mgc:\n.incbin "{data}"\n'
                           f'.globl magic_mgc_len\nmagic_mgc_len:\n.long {data.stat().st_size}\n.section .note.GNU-stack,"",@progbits\n')
            subprocess.run([config["cc"], "-c", str(asm), "-o", str(tmp / "magic.o")], check=True)
            objects.append(str(tmp / "magic.o"))

        libs = {"im": ["-lMagickWand-7.Q16HDRI", "-lMagickCore-7.Q16HDRI"], "pdf": ["-lpdfium"],
                "djvu": ["-ldjvulibre"], "svg": ["-lsvg", "-lpng", "-ljpeg", "-lwebpdemux", "-lwebp", "-lgif", "-lz"], "magic": ["-lmagic"]}
        subprocess.run([config["cxx"], *common, *objects, "-o", str(output), *libs[kind], *config["ldflags"], "-Wl,--gc-sections", "-lm"], check=True)


if __name__ == "__main__":
    main()
