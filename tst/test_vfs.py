#!/usr/bin/env python3
"""Paths resolve as Linux resolves them. One probe of the walk, run by
python in rootfs and under a root of the host, gives what Linux gives:
slashes at the end, symbolic links (followed, not, created through, too
many), "." and ".." (at a mount's root, past the root, after a link),
names that are mounts, the working directory and directory descriptors
as directories through renames and removals, the length of names and
paths, the names a call refuses, open's flags, and calls on a link
itself. Every case starts in a directory of its own under /tmp.

    test_vfs.py <vm> <rootfs> <python3>
    test_vfs.py --linux

LINUX is what Linux answers: `test_vfs.py --linux` runs the probe in a
chroot of a user namespace whose root, read-only, has the image's /etc
and /usr -> /, and a tmpfs at /tmp, with the host's python. rootfs must
answer it line for line. A root of the host has no mount at /tmp and
keeps its names by permissions, not by a read-only mount: there the
cases of HOST_ROOT answer as it says.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

PROBE = r'''import errno, os, stat, sys

CASES = []

def case(name):
    def deco(f):
        CASES.append((name, f))
        return f
    return deco

def kind(p, follow=True, **kw):
    return stat.filemode(os.stat(p, follow_symlinks=follow, **kw).st_mode)[0]

def ident(p, **kw):
    st = os.stat(p, **kw)
    return (st.st_dev, st.st_ino)

def same(a, b):
    return ident(a) == ident(b)

def create(p, flags=0, **kw):
    os.close(os.open(p, os.O_WRONLY | os.O_CREAT | flags, 0o644, **kw))

def exists(p):
    return os.path.lexists(p)

def err(f):
    try:
        f()
        return "ok"
    except OSError as e:
        return errno.errorcode.get(e.errno, str(e.errno))

def fixture(t):
    os.mkdir(t)
    os.chdir(t)
    with open("f", "w") as out:
        out.write("f")
    os.mkdir("d")
    create("d/x")
    os.symlink("d", "ld")
    os.symlink("f", "lf")
    os.symlink("missing", "dl")
    os.symlink("/etc", "abs")
    os.symlink("..", "up")
    os.symlink("../../..", "up3")
    os.symlink("loop", "loop")
    os.symlink(".", "dot")
    os.symlink(t + "/d", "absself")
    os.symlink("d/", "lslash")
    os.symlink("f/", "lfslash")

# A: a trailing slash
case("A01 stat f/")(lambda: kind("f/"))
case("A02 open f/")(lambda: os.close(os.open("f/", os.O_RDONLY)))
case("A03 lstat ld/")(lambda: kind("ld/", False))
case("A04 lstat lf/")(lambda: kind("lf/", False))
case("A05 open(O_CREAT) n/ ; n made")(lambda: (err(lambda: create("n/")), exists("n")))
case("A06 mkdir d2/")(lambda: os.mkdir("d2/"))
case("A07 unlink f/ ; f kept")(lambda: (err(lambda: os.unlink("f/")), exists("f")))
case("A08 unlink ld/ ; ld kept")(lambda: (err(lambda: os.unlink("ld/")), exists("ld")))
case("A09 rmdir ld/ ; ld kept")(lambda: (err(lambda: os.rmdir("ld/")), exists("ld")))
case("A10 rename f/ g ; f kept")(lambda: (err(lambda: os.rename("f/", "g")), exists("f")))
case("A11 rename f g/ ; f kept")(lambda: (err(lambda: os.rename("f", "g/")), exists("f")))
case("A12 symlink x s/ ; s made")(lambda: (err(lambda: os.symlink("x", "s/")), exists("s")))
case("A13 link f h/ ; h made")(lambda: (err(lambda: os.link("f", "h/")), exists("h")))
case("A14 mkfifo q/ ; q made")(lambda: (err(lambda: os.mkfifo("q/")), exists("q")))
case("A15 readlink ld/")(lambda: os.readlink("ld/"))
case("A16 rmdir d/ after emptying")(lambda: (os.unlink("d/x"), os.rmdir("d/"))[1])
case("A17 stat d/")(lambda: kind("d/"))
case("A18 chdir f/")(lambda: os.chdir("f/"))
case("A19 stat lslash (target d/)")(lambda: kind("lslash"))
case("A20 stat lfslash (target f/)")(lambda: kind("lfslash"))
case("A21 open(O_CREAT) dl/ ; missing made")(lambda: (err(lambda: create("dl/")), exists("missing")))
case("A22 stat d/x/ ")(lambda: kind("d/x/"))
case("A23 stat d/.")(lambda: kind("d/."))
case("A24 stat f/.")(lambda: kind("f/."))

# B: symbolic links
case("B01 open(O_CREAT) dangling dl ; dl kind, missing made")(lambda: (err(lambda: create("dl")), kind("dl", False), exists("missing")))
case("B02 open(O_CREAT|O_EXCL) dangling dl ; dl kind, missing made")(lambda: (err(lambda: create("dl", os.O_EXCL)), kind("dl", False), exists("missing")))
case("B03 open(O_NOFOLLOW) lf")(lambda: os.close(os.open("lf", os.O_RDONLY | os.O_NOFOLLOW)))
case("B04 open(O_NOFOLLOW) ld/x")(lambda: os.close(os.open("ld/x", os.O_RDONLY | os.O_NOFOLLOW)))
case("B05 open abs/motd (absolute link in /tmp)")(lambda: open("abs/motd").read().strip())
case("B06 stat up3/etc/motd (relative link past /tmp's root)")(lambda: kind("up3/etc/motd"))
case("B07 stat absself/x (absolute link back into /tmp)")(lambda: kind("absself/x"))
case("B08 stat loop")(lambda: kind("loop"))
def chain(n):
    os.symlink("f", "c0")
    for i in range(1, n):
        os.symlink(f"c{i - 1}", f"c{i}")
    return kind(f"c{n - 1}")
case("B09 stat a chain of 40 links")(lambda: chain(40))
case("B10 stat a chain of 41 links")(lambda: chain(41))
case("B11 stat dot*40/f (40 links in a row)")(lambda: kind("dot/" * 40 + "f"))
case("B12 stat dot*41/f (41 links in a row)")(lambda: kind("dot/" * 41 + "f"))
case("B13 mkdir over dangling dl")(lambda: os.mkdir("dl"))
case("B14 symlink over existing f")(lambda: os.symlink("x", "f"))
case("B15 open(O_DIRECTORY) ld")(lambda: os.close(os.open("ld", os.O_RDONLY | os.O_DIRECTORY)))
case("B16 open(O_DIRECTORY|O_NOFOLLOW) ld")(lambda: os.close(os.open("ld", os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)))
case("B17 chdir ld ; getcwd")(lambda: (os.chdir("ld"), os.getcwd())[1])
case("B18 chdir up3 ; getcwd")(lambda: (os.chdir("up3"), os.getcwd())[1])
case("B19 chdir abs ; getcwd")(lambda: (os.chdir("abs"), os.getcwd())[1])
case("B20 readlink f")(lambda: os.readlink("f"))
case("B21 listdir up3/usr/usr/etc")(lambda: sorted(os.listdir("up3/usr/usr/etc")))
case("B22 open(O_CREAT) through dangling into missing dir")(lambda: (os.symlink("nodir/file", "dl4"), create("dl4"))[1])
case("B23 open(O_CREAT) through abs link into /etc")(lambda: create("abs/new"))
case("B24 rmdir ld")(lambda: os.rmdir("ld"))
case("B25 unlink ld ; d kept")(lambda: (os.unlink("ld"), exists("d"))[1])

# C: ".."
case("C01 /tmp/.. is /")(lambda: same("/tmp/..", "/"))
case("C02 stat /tmp/../etc/motd")(lambda: kind("/tmp/../etc/motd"))
case("C03 chdir /tmp ; stat ../etc/motd")(lambda: (os.chdir("/tmp"), kind("../etc/motd"))[1])
case("C04 /.. is /")(lambda: same("/..", "/"))
case("C05 /usr/.. is /")(lambda: same("/usr/..", "/"))
case("C06 stat /etc/motd/..")(lambda: kind("/etc/motd/.."))
case("C07 stat /nonexistent/..")(lambda: kind("/nonexistent/.."))
case("C08 ld/.. is .")(lambda: same("ld/..", "."))
case("C09 abs/.. is /")(lambda: same("abs/..", "/"))
def c10():
    fd = os.open("abs/..", os.O_RDONLY | os.O_DIRECTORY)
    try:
        return kind("etc/motd", dir_fd=fd)
    finally:
        os.close(fd)
case("C10 openat(open(abs/..), etc/motd)")(c10)
def c11():
    fd = os.open("/tmp", os.O_RDONLY | os.O_DIRECTORY)
    try:
        return kind("../etc/motd", dir_fd=fd)
    finally:
        os.close(fd)
case("C11 openat(open(/tmp), ../etc/motd)")(c11)
case("C12 st_dev of /tmp/.. is /'s")(lambda: os.stat("/tmp/..").st_dev == os.stat("/").st_dev)
case("C13 st_dev of /tmp is not /'s")(lambda: os.stat("/tmp").st_dev != os.stat("/").st_dev)
case("C14 chdir /tmp ; chdir .. ; getcwd")(lambda: (os.chdir("/tmp"), os.chdir(".."), os.getcwd())[2])
case("C15 //etc//motd")(lambda: kind("//etc//motd"))
case("C16 stat /etc/.")(lambda: kind("/etc/."))

# D: the working directory and directory descriptors are objects
def d01():
    os.mkdir("w")
    os.chdir("w")
    os.rename("../w", "../w2")
    return (err(lambda: create("x")), exists("../w2/x"), os.getcwd().rsplit("/", 1)[-1])
case("D01 cwd renamed: create x ; made in w2 ; getcwd's last name")(d01)
def d02():
    os.mkdir("v")
    fd = os.open("v", os.O_RDONLY | os.O_DIRECTORY)
    os.rename("v", "v2")
    try:
        return (err(lambda: create("y", dir_fd=fd)), exists("v2/y"))
    finally:
        os.close(fd)
case("D02 dirfd renamed: openat y ; made in v2")(d02)
def d03():
    here = os.getcwd()
    os.mkdir("z")
    os.chdir("z")
    os.rmdir(here + "/z")
    return (err(os.getcwd), err(lambda: create("a")))
case("D03 cwd removed: getcwd ; create")(d03)
def d04():
    here = os.getcwd()
    os.mkdir("k")
    os.chdir("k")
    os.rename(here + "/k", here + "/k_old")
    os.mkdir(here + "/k")
    return (err(lambda: create("m")), exists(here + "/k_old/m"), exists(here + "/k/m"))
case("D04 cwd replaced by a new dir of its name: create m ; in k_old ; in new k")(d04)
def d05():
    fd = os.open("/tmp", os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fchdir(fd)
        return os.getcwd()
    finally:
        os.close(fd)
case("D05 fchdir /tmp ; getcwd")(d05)
def d06():
    fd = os.open("ld", os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fchdir(fd)
        return os.getcwd().rsplit("/", 1)[-1]
    finally:
        os.close(fd)
case("D06 fchdir open(ld) ; getcwd's last name")(d06)

# E: names
case("E01 create a name of 256")(lambda: create("a" * 256))
case("E02 create a name of 255")(lambda: create("a" * 255))
case("E03 stat empty")(lambda: kind(""))
case("E04 stat a path of 4096")(lambda: kind("/" + "./" * 2047 + "."))
case("E05 stat a path of 4095")(lambda: kind("/" + "./" * 2047))
def e06():
    for i in range(22):
        name = "%03d" % i + "n" * 197
        os.mkdir(name)
        os.chdir(name)
    return (err(lambda: create("leaf")), err(os.getcwd))
case("E06 below a cwd longer than 4096: create ; getcwd")(e06)

# F: special names, mount points
case("F01 rmdir d/.")(lambda: os.rmdir("d/."))
case("F02 rmdir d/..")(lambda: os.rmdir("d/.."))
case("F03 unlink d/.")(lambda: os.unlink("d/."))
case("F04 mkdir d/.")(lambda: os.mkdir("d/."))
case("F05 rename d d/sub")(lambda: os.rename("d", "d/sub"))
case("F06 rename d/. x")(lambda: os.rename("d/.", "x"))
case("F07 rmdir /tmp")(lambda: os.rmdir("/tmp"))
case("F08 rename f /f")(lambda: os.rename("f", "/f"))
case("F09 link f /f2")(lambda: os.link("f", "/f2"))
case("F10 rename /tmp /tmp2")(lambda: os.rename("/tmp", "/tmp2"))
case("F11 link d d3")(lambda: os.link("d", "d3"))
case("F12 link lf hl, following ; hl is f")(lambda: (os.link("lf", "hl", follow_symlinks=True), same("hl", "f"))[1])
case("F13 link lf hl2, not following ; hl2 kind")(lambda: (os.link("lf", "hl2", follow_symlinks=False), kind("hl2", False))[1])
case("F14 rmdir /")(lambda: os.rmdir("/"))
case("F15 unlink d/..")(lambda: os.unlink("d/.."))
case("F16 rename f lf (onto a link) ; lf kind")(lambda: (os.rename("f", "lf"), kind("lf", False))[1])
case("F17 open(O_CREAT|O_WRONLY) .")(lambda: create("."))

# G: open flags
case("G01 open(O_DIRECTORY) /etc/motd")(lambda: os.close(os.open("/etc/motd", os.O_RDONLY | os.O_DIRECTORY)))
case("G02 open(O_DIRECTORY|O_NOFOLLOW) /usr")(lambda: os.close(os.open("/usr", os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)))
case("G03 open(O_WRONLY) d")(lambda: os.close(os.open("d", os.O_WRONLY)))
case("G04 listdir ld")(lambda: sorted(os.listdir("ld")))
case("G05 listdir /usr/usr/etc")(lambda: sorted(os.listdir("/usr/usr/etc")))

# H: the link itself
case("H01 access dl F_OK")(lambda: os.access("dl", os.F_OK))
case("H02 access dl F_OK, not following")(lambda: os.access("dl", os.F_OK, follow_symlinks=False))
case("H03 chmod lf 600, not following ; f's mode")(lambda: (err(lambda: os.chmod("lf", 0o600, follow_symlinks=False)), oct(os.stat("f").st_mode & 0o777)))
case("H04 utime lf, not following ; lf's mtime, f's is 1")(lambda: (os.utime("lf", (1, 1), follow_symlinks=False), int(os.lstat("lf").st_mtime), int(os.stat("f").st_mtime) == 1)[1:])
case("H05 lchown lf")(lambda: os.chown("lf", -1, -1, follow_symlinks=False))

for i, (name, f) in enumerate(CASES):
    t = "/tmp/p%03d" % i
    try:
        fixture(t)
        v = f()
        print(f"{name}: ok" + ("" if v is None else f" {v}"))
    except OSError as e:
        print(f"{name}: {errno.errorcode.get(e.errno, e.errno)}")
    except NotImplementedError:
        print(f"{name}: n/a")
    except Exception as e:
        print(f"{name}: {type(e).__name__} {e}")
    finally:
        os.chdir("/")
'''

LINUX = """\
A01 stat f/: ENOTDIR
A02 open f/: ENOTDIR
A03 lstat ld/: ok d
A04 lstat lf/: ENOTDIR
A05 open(O_CREAT) n/ ; n made: ok ('EISDIR', False)
A06 mkdir d2/: ok
A07 unlink f/ ; f kept: ok ('ENOTDIR', True)
A08 unlink ld/ ; ld kept: ok ('ENOTDIR', True)
A09 rmdir ld/ ; ld kept: ok ('ENOTDIR', True)
A10 rename f/ g ; f kept: ok ('ENOTDIR', True)
A11 rename f g/ ; f kept: ok ('ENOTDIR', True)
A12 symlink x s/ ; s made: ok ('ENOENT', False)
A13 link f h/ ; h made: ok ('ENOENT', False)
A14 mkfifo q/ ; q made: ok ('ENOENT', False)
A15 readlink ld/: EINVAL
A16 rmdir d/ after emptying: ok
A17 stat d/: ok d
A18 chdir f/: ENOTDIR
A19 stat lslash (target d/): ok d
A20 stat lfslash (target f/): ENOTDIR
A21 open(O_CREAT) dl/ ; missing made: ok ('EISDIR', False)
A22 stat d/x/ : ENOTDIR
A23 stat d/.: ok d
A24 stat f/.: ENOTDIR
B01 open(O_CREAT) dangling dl ; dl kind, missing made: ok ('ok', 'l', True)
B02 open(O_CREAT|O_EXCL) dangling dl ; dl kind, missing made: ok ('EEXIST', 'l', False)
B03 open(O_NOFOLLOW) lf: ELOOP
B04 open(O_NOFOLLOW) ld/x: ok
B05 open abs/motd (absolute link in /tmp): ok pizdos
B06 stat up3/etc/motd (relative link past /tmp's root): ok -
B07 stat absself/x (absolute link back into /tmp): ok -
B08 stat loop: ELOOP
B09 stat a chain of 40 links: ok -
B10 stat a chain of 41 links: ELOOP
B11 stat dot*40/f (40 links in a row): ok -
B12 stat dot*41/f (41 links in a row): ELOOP
B13 mkdir over dangling dl: EEXIST
B14 symlink over existing f: EEXIST
B15 open(O_DIRECTORY) ld: ok
B16 open(O_DIRECTORY|O_NOFOLLOW) ld: ENOTDIR
B17 chdir ld ; getcwd: ok /tmp/p040/d
B18 chdir up3 ; getcwd: ok /
B19 chdir abs ; getcwd: ok /etc
B20 readlink f: EINVAL
B21 listdir up3/usr/usr/etc: ok ['group', 'motd', 'passwd']
B22 open(O_CREAT) through dangling into missing dir: ENOENT
B23 open(O_CREAT) through abs link into /etc: EROFS
B24 rmdir ld: ENOTDIR
B25 unlink ld ; d kept: ok True
C01 /tmp/.. is /: ok True
C02 stat /tmp/../etc/motd: ok -
C03 chdir /tmp ; stat ../etc/motd: ok -
C04 /.. is /: ok True
C05 /usr/.. is /: ok True
C06 stat /etc/motd/..: ENOTDIR
C07 stat /nonexistent/..: ENOENT
C08 ld/.. is .: ok True
C09 abs/.. is /: ok True
C10 openat(open(abs/..), etc/motd): ok -
C11 openat(open(/tmp), ../etc/motd): ok -
C12 st_dev of /tmp/.. is /'s: ok True
C13 st_dev of /tmp is not /'s: ok True
C14 chdir /tmp ; chdir .. ; getcwd: ok /
C15 //etc//motd: ok -
C16 stat /etc/.: ok d
D01 cwd renamed: create x ; made in w2 ; getcwd's last name: ok ('ok', True, 'w2')
D02 dirfd renamed: openat y ; made in v2: ok ('ok', True)
D03 cwd removed: getcwd ; create: ok ('ENOENT', 'ENOENT')
D04 cwd replaced by a new dir of its name: create m ; in k_old ; in new k: ok ('ok', True, False)
D05 fchdir /tmp ; getcwd: ok /tmp
D06 fchdir open(ld) ; getcwd's last name: ok d
E01 create a name of 256: ENAMETOOLONG
E02 create a name of 255: ok
E03 stat empty: ENOENT
E04 stat a path of 4096: ENAMETOOLONG
E05 stat a path of 4095: ok d
E06 below a cwd longer than 4096: create ; getcwd: ok ('ok', 'ENAMETOOLONG')
F01 rmdir d/.: EINVAL
F02 rmdir d/..: ENOTEMPTY
F03 unlink d/.: EISDIR
F04 mkdir d/.: EEXIST
F05 rename d d/sub: EINVAL
F06 rename d/. x: EBUSY
F07 rmdir /tmp: EROFS
F08 rename f /f: EXDEV
F09 link f /f2: EROFS
F10 rename /tmp /tmp2: EROFS
F11 link d d3: EPERM
F12 link lf hl, following ; hl is f: ok True
F13 link lf hl2, not following ; hl2 kind: ok l
F14 rmdir /: EBUSY
F15 unlink d/..: EISDIR
F16 rename f lf (onto a link) ; lf kind: ok -
F17 open(O_CREAT|O_WRONLY) .: EISDIR
G01 open(O_DIRECTORY) /etc/motd: ENOTDIR
G02 open(O_DIRECTORY|O_NOFOLLOW) /usr: ENOTDIR
G03 open(O_WRONLY) d: EISDIR
G04 listdir ld: ok ['x']
G05 listdir /usr/usr/etc: ok ['group', 'motd', 'passwd']
H01 access dl F_OK: ok False
H02 access dl F_OK, not following: ok True
H03 chmod lf 600, not following ; f's mode: n/a
H04 utime lf, not following ; lf's mtime, f's is 1: ok (1, False)
H05 lchown lf: ok
"""

# The cases a root of the host answers otherwise, for what it is: /tmp is
# no mount there, and / and /etc are closed by their modes.
HOST_ROOT = {
    "B23": "EACCES",
    "C13": "ok False",
    "F07": "EACCES",
    "F08": "EACCES",
    "F09": "EACCES",
    "F10": "EACCES",
}

FILES = {"etc/motd": "pizdos\n", "etc/passwd": "root:x:0:0:root:/:/bin/sh\n", "etc/group": "root:x:0:\n"}


def compare(got, want, where):
    got, want = got.splitlines(), want.splitlines()
    differ = [f"{w}\n    {where}: {g.partition(': ')[2]}" for g, w in zip(got, want) if g != w]
    assert len(got) == len(want) and not differ, (where, len(got), len(want), "\n".join(differ))


def host_root(want):
    lines = []
    for line in want.splitlines():
        name, _, answer = line.partition(": ")
        lines.append(f"{name}: {HOST_ROOT.get(name[:3], answer)}")
    return "\n".join(lines) + "\n"


def linux():
    """The probe's answers on this host's Linux, as LINUX holds them."""
    python = os.path.realpath(sys.executable)
    store = "/" + Path(python).parts[1]
    with tempfile.TemporaryDirectory(prefix="pizdos-vfs-linux-") as work:
        root = Path(work)
        for d in ("etc", "tmp", "dev", store.lstrip("/")):
            (root / d).mkdir(parents=True, exist_ok=True)
        for name, text in FILES.items():
            (root / name).write_text(text)
        (root / "usr").symlink_to("/")
        (root / "dev/null").touch()
        (root / "probe.py").write_text(PROBE)
        script = (f"mount --bind {root} {root} && mount -o remount,bind,ro {root} && mount -t tmpfs none {root}/tmp && "
                  f"mount --rbind {store} {root}{store} && mount --bind /dev/null {root}/dev/null && "
                  f"chroot {root} {python} -I /probe.py")
        result = subprocess.run(["unshare", "--user", "--map-root-user", "--mount", "--fork", "sh", "-c", script],
                                capture_output=True, text=True, timeout=600)
        assert result.returncode == 0 and result.stderr == "", result
        return result.stdout


def main():
    if sys.argv[1:] == ["--linux"]:
        print(linux(), end="")
        return
    vm, rootfs, python = sys.argv[1:4]
    env = {"W2X_MEMORY_TAIL": "strict", "W2X_TAIL_PROFILE": ""}
    with tempfile.TemporaryDirectory(prefix="pizdos-vfs-") as work:
        script = "python3 - <<'PROBE_EOF'\n" + PROBE + "PROBE_EOF\nexit 0\n"
        result = subprocess.run([vm, "run", "--", rootfs], input=script.encode(), capture_output=True, cwd=work, env=env,
                                timeout=600)
        assert result.returncode == 0 and result.stderr == b"", result
        compare(result.stdout.decode(), LINUX, "rootfs")

        root = Path(work) / "root"
        for d in ("bin", "etc", "tmp"):
            (root / d).mkdir(parents=True)
        for name, text in FILES.items():
            (root / name).write_text(text)
        (root / "usr").symlink_to("/")
        (root / "bin/python3").write_bytes(Path(python).read_bytes())
        (root / "bin/python3").chmod(0o755)
        (root / "probe.py").write_text(PROBE)
        for d in (root, root / "etc"):
            d.chmod(0o555)
        try:
            result = subprocess.run([vm, "run", "--root", root, "-a", "/bin/python3", "--", root / "bin/python3", "/probe.py"],
                                    capture_output=True, cwd=work, env=env, timeout=600)
        finally:
            for d in (root, root / "etc"):
                d.chmod(0o755)
        assert result.returncode == 0 and result.stderr == b"", result
        compare(result.stdout.decode(), host_root(LINUX), "host root")
    print(f"vfs: {len(LINUX.splitlines())} cases of the walk answer as Linux's in rootfs and under a root of the host")


if __name__ == "__main__":
    main()
