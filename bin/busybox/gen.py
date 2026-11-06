#!/usr/bin/env python3
"""Generate busybox's configuration-dependent sources inside an unpacked tree.

    gen.py <busybox-tree> <config> <objects-out> <links-out>

Writes into <busybox-tree>/include the headers busybox's own build would
generate (autoconf.h, applets.h, usage.h, applet_tables.h, NUM_APPLETS.h,
usage_compressed.h, common_bufsiz.h, embedded_scripts.h) and lists in
<objects-out> the objects the configuration selects, one `lib|obj dir/name.o`
per line, and in <links-out> the install path of every applet link, like
busybox.links. Nothing is compiled for the host: kconfig is replaced by reading the
symbol declarations and taking <config> literally, the applet and usage tables
are evaluated by the target compiler's preprocessor (CC, default clang).
"""
import os
import re
import subprocess
import sys
from pathlib import Path

CORE_DIRS = ["applets"]

LIB_DIRS = [
    "archival",
    "archival/libarchive",
    "console-tools",
    "coreutils",
    "coreutils/libcoreutils",
    "debianutils",
    "klibc-utils",
    "e2fsprogs",
    "editors",
    "findutils",
    "init",
    "libbb",
    "libpwdgrp",
    "loginutils",
    "mailutils",
    "miscutils",
    "modutils",
    "networking",
    "networking/libiproute",
    "networking/udhcp",
    "printutils",
    "procps",
    "runit",
    "selinux",
    "shell",
    "sysklogd",
    "util-linux",
    "util-linux/volume_id",
]

SYMBOL = re.compile(r"^\s*(?:menu)?config\s+([A-Za-z0-9_]+)\s*$")
TYPE = re.compile(r"^\s*(bool|tristate|int|hex|string)\b")
DEFAULT = re.compile(r"^\s*default\s+(.*?)\s*$")
DEFAULT_VALUE = re.compile(r'^("(?:[^"\\]|\\.)*"|[^\s#]+)')
HELP = re.compile(r"^\s*(?:help|---help---)\s*$")
SELECT = re.compile(r"^\s*select\s+([A-Za-z0-9_]+)\s*$")
VALUE = re.compile(r"^CONFIG_([A-Za-z0-9_]+)=(.*)$")
UNSET = re.compile(r"^# CONFIG_([A-Za-z0-9_]+) is not set$")
ASSIGN = re.compile(r"^([A-Za-z0-9_-]+)\s*(:=|\+=|=)\s*(.*)$")
CONDITION = re.compile(r"^(ifeq|ifneq)\s*\((.*),(.*)\)$")
APPLET_ENTRY = re.compile(r'\{\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,\s*(\w+)\s*,\s*(\w+)\s*(?:,\s*(\d+)\s*)?(?:,\s*(\d+)\s*)?\}')

INSTALL_LOCATIONS = {
    "BB_DIR_ROOT": 0,
    "BB_DIR_BIN": 1,
    "BB_DIR_SBIN": 2,
    "BB_DIR_USR_BIN": 3,
    "BB_DIR_USR_SBIN": 4,
}

SUID_KINDS = {
    "BB_SUID_DROP": 0,
    "BB_SUID_MAYBE": 1,
    "BB_SUID_REQUIRE": 2,
}


def sources(tree, pattern):
    return sorted(str(path.relative_to(tree)) for path in tree.glob(pattern))


def tagged_lines(tree, files, tag):
    lines = []

    for name in files:
        for line in (tree / name).read_text(encoding="latin-1").splitlines():
            if line.startswith(tag):
                lines.append(line[len(tag):])

    return lines


def kconfig_dirs(tree):
    return sorted(str(path.parent.relative_to(tree)) for path in tree.rglob("Config.src"))


def declarations(tree):
    texts = [(tree / "Config.in").read_text(encoding="latin-1").splitlines()]

    for directory in kconfig_dirs(tree):
        files = sources(tree, f"{directory}/*.c")
        inserted = tagged_lines(tree, files, "//config:")
        texts.append(spliced(tree / directory / "Config.src", inserted))

    symbols = {}

    for lines in texts:
        current = None
        attributes = True

        for line in lines:
            match = SYMBOL.match(line)

            if match:
                current = symbols.setdefault(match.group(1), {"type": None, "defaults": [], "selects": []})
                attributes = True

                continue

            if current is None or not attributes:
                continue

            if HELP.match(line):
                attributes = False

                continue

            match = TYPE.match(line)

            if match and current["type"] is None:
                current["type"] = match.group(1)

                continue

            match = SELECT.match(line)

            if match:
                current["selects"].append(match.group(1))

                continue

            match = DEFAULT.match(line)

            if match:
                current["defaults"].append(match.group(1))

    return symbols


def literal_default(symbol):
    for candidate in symbol["defaults"]:
        match = DEFAULT_VALUE.match(candidate)
        value = match.group(1) if match else ""

        if symbol["type"] == "string" and value.startswith('"'):
            return value[1:-1].replace('\\"', '"').replace("\\\\", "\\")

        if symbol["type"] in ("int", "hex") and re.fullmatch(r"-?(0x)?[0-9A-Fa-f]+", value):
            return value

    return ""


def read_config(path, symbols):
    values = {}

    for number, line in enumerate(Path(path).read_text().splitlines(), 1):
        line = line.strip()

        if not line:
            continue

        match = UNSET.match(line)

        if match:
            name, value = match.group(1), "n"
        elif line.startswith("#"):
            continue
        else:
            match = VALUE.match(line)

            if not match:
                raise SystemExit(f"{path}:{number}: cannot parse {line!r}")

            name, value = match.group(1), match.group(2)

        if name not in symbols:
            raise SystemExit(f"{path}:{number}: CONFIG_{name} is not a busybox symbol")

        if symbols[name]["type"] == "string":
            if not (value.startswith('"') and value.endswith('"')):
                raise SystemExit(f"{path}:{number}: CONFIG_{name} needs a quoted string")

            value = value[1:-1].replace('\\"', '"').replace("\\\\", "\\")

        values[name] = value

    return values


def selected(symbols, values):
    values = dict(values)
    pending = [name for name, value in values.items() if value == "y"]

    while pending:
        name = pending.pop()

        for target in symbols[name]["selects"]:
            if target not in symbols:
                raise SystemExit(f"CONFIG_{name} selects unknown CONFIG_{target}")

            if values.get(target) == "n":
                raise SystemExit(f"CONFIG_{name} selects CONFIG_{target}, which the config disables")

            if values.get(target) != "y":
                values[target] = "y"
                pending.append(target)

    return values


def resolve(symbols, values):
    values = selected(symbols, values)
    resolved = {}

    for name, symbol in symbols.items():
        kind = symbol["type"] or "bool"

        if kind in ("bool", "tristate"):
            value = values.get(name, "n")

            if value not in ("y", "n"):
                raise SystemExit(f"CONFIG_{name} is a {kind}, not {value!r}")

            resolved[name] = (kind, value)
        else:
            resolved[name] = (kind, values.get(name, literal_default(symbol)))

    return resolved


def enabled_block(name):
    return [
        f"#define ENABLE_{name} 1",
        "#ifdef MAKE_SUID",
        f'# define IF_{name}(...) __VA_ARGS__ "CONFIG_{name}"',
        "#else",
        f"# define IF_{name}(...) __VA_ARGS__",
        "#endif",
        f"#define IF_NOT_{name}(...)",
    ]


def autoconf(config):
    out = [
        "/*",
        " * Automatically generated C config: don't edit",
        " */",
        '#define AUTOCONF_TIMESTAMP ""',
        "",
    ]

    for name, (kind, value) in config.items():
        if kind in ("bool", "tristate"):
            if value == "y":
                out.append(f"#define CONFIG_{name} 1")
                out.extend(enabled_block(name))
            else:
                out.append(f"#undef CONFIG_{name}")
                out.append(f"#define ENABLE_{name} 0")
                out.append(f"#define IF_{name}(...)")
                out.append(f"#define IF_NOT_{name}(...) __VA_ARGS__")

            continue

        if kind == "string":
            escaped = value.replace("\\", "\\\\").replace('"', '\\"')
            out.append(f'#define CONFIG_{name} "{escaped}"')
        elif kind == "hex" and not value.lower().startswith("0x"):
            out.append(f"#define CONFIG_{name} 0x{value}")
        else:
            out.append(f"#define CONFIG_{name} {value or '0'}")

        out.extend(enabled_block(name))

    return "\n".join(out) + "\n"


def spliced(template, inserted):
    lines = template.read_text(encoding="latin-1").splitlines()

    if "INSERT" not in lines:
        return lines

    at = lines.index("INSERT")

    return lines[:at] + inserted + lines[at + 1:]


def generated(template, inserted, header):
    return "\n".join([header] + spliced(template, inserted)) + "\n"


def usage_lines(lines):
    out = []

    for line in lines:
        if line.startswith((" ", "\t")):
            out.append(line + " \\")
        else:
            out.append("")
            out.append(line + " \\")

    return out


def substitute(text, config, variables):
    def value(match):
        name = match.group(1)

        if name.startswith("CONFIG_"):
            kind, setting = config.get(name[len("CONFIG_"):], ("bool", "n"))

            return "y" if kind in ("bool", "tristate") and setting == "y" else ""

        return " ".join(variables.get(name, []))

    return re.sub(r"\$\(([A-Za-z0-9_-]+)\)", value, text)


def logical_lines(lines):
    pending = ""

    for line in lines:
        line = line.split("#", 1)[0].rstrip()

        if line.endswith("\\"):
            pending += line[:-1] + " "

            continue

        yield (pending + line).strip()
        pending = ""

    if pending.strip():
        yield pending.strip()


def kbuild_objects(tree, directory, config):
    template = tree / directory / "Kbuild.src"

    if not template.exists():
        return [], []

    files = sources(tree, f"{directory}/*.c")
    lines = spliced(template, tagged_lines(tree, files, "//kbuild:"))
    variables = {"lib-y": [], "obj-y": []}
    active = [True]

    for line in logical_lines(lines):
        if not line:
            continue

        match = CONDITION.match(line)

        if match:
            left = substitute(match.group(2), config, variables).strip()
            right = substitute(match.group(3), config, variables).strip()
            holds = (left == right) == (match.group(1) == "ifeq")
            active.append(active[-1] and holds)

            continue

        if line == "else":
            parent = active[-2] if len(active) > 1 else True
            active[-1] = parent and not active[-1]

            continue

        if line == "endif":
            active.pop()

            continue

        if not active[-1]:
            continue

        match = ASSIGN.match(substitute(line, config, variables))

        if not match:
            continue

        words = match.group(3).split()

        if match.group(2) == "+=":
            variables[match.group(1)] = variables.get(match.group(1), []) + words
        else:
            variables[match.group(1)] = words

    obj = list(dict.fromkeys(variables["obj-y"]))
    lib = [name for name in dict.fromkeys(variables["lib-y"]) if name not in obj]

    return lib, obj


def preprocess(tree, text):
    cc = os.environ.get("CC", "clang")
    source = tree / "include" / ".gen_probe.c"
    source.write_text(text)

    try:
        return subprocess.run(
            [cc, "--target=wasm32-unknown-unknown", "-E", "-P", "-nostdinc", "-I", str(tree / "include"), str(source)],
            check=True,
            capture_output=True,
            text=True,
        ).stdout
    finally:
        source.unlink()


def enabled(config, name):
    kind, value = config[name]

    return value == "y"


def applet_table(tree, config):
    out = preprocess(tree, '#include "autoconf.h"\n#include "applet_metadata.h"\n#include "applets.h"\n')
    applets = []

    for match in APPLET_ENTRY.finditer(out):
        name, main, location, suid, noexec, nofork = match.groups()

        if location not in INSTALL_LOCATIONS or suid not in SUID_KINDS:
            raise SystemExit(f"applet {name}: unknown location {location} or suid {suid}")

        applets.append({
            "name": name,
            "main": main,
            "location": INSTALL_LOCATIONS[location],
            "suid": SUID_KINDS[suid],
            "noexec": int(noexec or 0),
            "nofork": int(nofork or 0),
        })

    applets.sort(key=lambda applet: applet["name"].encode())
    names = [applet["name"] for applet in applets]
    duplicates = sorted({name for name in names if names.count(name) > 1})

    if duplicates:
        raise SystemExit(f"duplicate applet names: {', '.join(duplicates)}")

    count = len(applets)
    offsets_known = 8

    if count < 128:
        offsets_known = 4

    if count < 32:
        offsets_known = 0

    lines = ["/* This is a generated file, don't edit */", "", f"#define NUM_APPLETS {count}"]

    if count == 1:
        lines.append(f'#define SINGLE_APPLET_STR "{applets[0]["name"]}"')
        lines.append(f'#define SINGLE_APPLET_MAIN {applets[0]["main"]}_main')

    lines.append(f"#define KNOWN_APPNAME_OFFSETS {offsets_known}")
    lines.append("")

    if offsets_known:
        index = [i * count // offsets_known for i in range(offsets_known)]
        offset = [0] * offsets_known
        position = 0

        for i, applet in enumerate(applets):
            for j in range(offsets_known):
                if i == index[j]:
                    offset[j] = position

            position += len(applet["name"]) + 1

        if position > 0xFFFF:
            raise SystemExit("applet names do not fit 16-bit offsets")

        lines.append("const uint16_t applet_nameofs[] ALIGN2 = {")
        lines.extend(f"{value}," for value in offset[1:])
        lines.append("};")
        lines.append("")

    lines.append('const char applet_names[] ALIGN1 = ""')
    lines.extend(f'"{applet["name"]}" "\\0"' for applet in applets)
    lines.append(";")
    lines.append("")

    for i, applet in enumerate(applets):
        if re.fullmatch(r"[A-Za-z0-9_]+", applet["name"]):
            lines.append(f'#define APPLET_NO_{applet["name"]} {i}')

    lines.append("")
    lines.append("#ifndef SKIP_applet_main")
    lines.append("int (*const applet_main[])(int argc, char **argv) = {")
    lines.extend(f'{applet["main"]}_main,' for applet in applets)
    lines.append("};")
    lines.append("#endif")
    lines.append("")

    if enabled(config, "FEATURE_PREFER_APPLETS") or enabled(config, "FEATURE_SH_STANDALONE") or enabled(config, "FEATURE_SH_NOFORK"):
        lines.append("const uint8_t applet_flags[] ALIGN1 = {")
        lines.extend(packed(applets, 4, lambda applet: applet["nofork"] + (applet["noexec"] << 1)))
        lines.append("};")
        lines.append("")

    if enabled(config, "FEATURE_SUID"):
        lines.append("const uint8_t applet_suid[] ALIGN1 = {")
        lines.extend(packed(applets, 4, lambda applet: applet["suid"]))
        lines.append("};")
        lines.append("")

    if enabled(config, "FEATURE_INSTALLER"):
        lines.append("const uint8_t applet_install_loc[] ALIGN1 = {")
        lines.extend(packed(applets, 2, lambda applet: applet["location"]))
        lines.append("};")

    return "\n".join(lines) + "\n", count


def packed(applets, per_byte, field):
    shift = 8 // per_byte
    out = []

    for start in range(0, len(applets), per_byte):
        value = 0

        for k, applet in enumerate(applets[start:start + per_byte]):
            value |= field(applet) << (k * shift)

        out.append(f"0x{value:02x},")

    return out


STRING_TOKEN = re.compile(r'"((?:[^"\\\n]|\\.)*)"')

ESCAPES = {
    "n": "\n", "t": "\t", "r": "\r", "a": "\a", "b": "\b", "f": "\f", "v": "\v",
    "\\": "\\", '"': '"', "'": "'", "?": "?",
}


def unescape(body):
    out = bytearray()
    i = 0

    while i < len(body):
        char = body[i]

        if char != "\\":
            out += char.encode("latin-1")
            i += 1

            continue

        follower = body[i + 1]

        if follower in ESCAPES:
            out += ESCAPES[follower].encode("latin-1")
            i += 2
        elif follower in "01234567":
            digits = re.match(r"[0-7]{1,3}", body[i + 1:]).group(0)
            out.append(int(digits, 8) & 0xFF)
            i += 1 + len(digits)
        elif follower == "x":
            digits = re.match(r"[0-9A-Fa-f]+", body[i + 2:]).group(0)
            out.append(int(digits, 16) & 0xFF)
            i += 2 + len(digits)
        else:
            raise SystemExit(f"unknown escape \\{follower} in usage text")

    return bytes(out)


def usage_texts(tree, config):
    if not enabled(config, "SHOW_USAGE"):
        return b""

    nommu = enabled(config, "NOMMU")
    probe = "\n".join([
        '#include "autoconf.h"',
        f"#define BB_MMU {0 if nommu else 1}",
        f"#define USE_FOR_NOMMU(...) {'__VA_ARGS__' if nommu else ''}",
        f"#define USE_FOR_MMU(...) {'' if nommu else '__VA_ARGS__'}",
        '#include "usage.h"',
        "#define MAKE_USAGE(aname, usage) BBUSAGE aname usage BBEND",
        '#include "applets.h"',
        "",
    ])
    out = preprocess(tree, probe)
    entries = []

    for chunk in out.split("BBUSAGE")[1:]:
        body = chunk.split("BBEND", 1)[0]
        literals = [unescape(token) for token in STRING_TOKEN.findall(body)]
        entries.append((literals[0], b"".join(literals[1:])))

    entries.sort(key=lambda entry: entry[0])

    return b"".join(text + b"\0" for name, text in entries)


def c_literal(data):
    out = []

    for byte in data:
        char = chr(byte)

        if 0x20 <= byte < 0x7F and char not in '"\\?':
            out.append(char)
        else:
            out.append(f"\\{byte:03o}")

    return '"' + "".join(out) + '"'


def usage_header(texts):
    return "\n".join([
        f"#define UNPACKED_USAGE {c_literal(texts)}",
        f"#define UNPACKED_USAGE_LENGTH {len(texts)}",
        "",
        "#define PACKED_USAGE",
        "",
    ])


LINK_ENTRY = re.compile(r"\bLINK\s+(\w+)\s+(\S+)")

LINK_DIRECTORIES = {
    "BB_DIR_ROOT": "/",
    "BB_DIR_BIN": "/bin/",
    "BB_DIR_SBIN": "/sbin/",
    "BB_DIR_USR_BIN": "/usr/bin/",
    "BB_DIR_USR_SBIN": "/usr/sbin/",
}


def links(tree):
    out = preprocess(tree, '#include "autoconf.h"\n#define MAKE_LINKS\n#include "applets.h"\n')

    return sorted(LINK_DIRECTORIES[location] + name for location, name in LINK_ENTRY.findall(out))


def unsupported(config):
    for name in ("FEATURE_SH_EMBEDDED_SCRIPTS", "FEATURE_COMPRESS_USAGE", "BBCONFIG", "FEATURE_USE_BSS_TAIL"):
        if name in config and enabled(config, name):
            raise SystemExit(f"CONFIG_{name}=y needs a generator this build does not have")


def main():
    tree = Path(sys.argv[1])
    symbols = declarations(tree)
    config = resolve(symbols, read_config(sys.argv[2], symbols))
    unsupported(config)
    include = tree / "include"
    (include / "autoconf.h").write_text(autoconf(config))
    files = sources(tree, "*/*.c") + sources(tree, "*/*/*.c")
    (include / "applets.h").write_text(generated(
        include / "applets.src.h",
        tagged_lines(tree, files, "//applet:"),
        "/* DO NOT EDIT. This file is generated from applets.src.h */",
    ))
    (include / "usage.h").write_text(generated(
        include / "usage.src.h",
        usage_lines(tagged_lines(tree, files, "//usage:")),
        "/* DO NOT EDIT. This file is generated from usage.src.h */",
    ))
    table, count = applet_table(tree, config)
    (include / "applet_tables.h").write_text(table)
    (include / "NUM_APPLETS.h").write_text(f"#define NUM_APPLETS {count}\n")
    (include / "usage_compressed.h").write_text(usage_header(usage_texts(tree, config)))
    (include / "common_bufsiz.h").write_text(
        "enum { COMMON_BUFSIZE = 1024 };\n"
        "extern char bb_common_bufsiz1[];\n"
        "#define setup_common_bufsiz() ((void)0)\n"
    )
    (include / "embedded_scripts.h").write_text("#define NUM_SCRIPTS 0\n")
    objects = []

    for directory in CORE_DIRS + LIB_DIRS:
        lib, obj = kbuild_objects(tree, directory, config)
        objects += [f"obj {directory}/{name}" for name in obj]
        objects += [f"lib {directory}/{name}" for name in lib]

    Path(sys.argv[3]).write_text("\n".join(objects) + "\n")
    Path(sys.argv[4]).write_text("\n".join(links(tree)) + "\n")


if __name__ == "__main__":
    main()
