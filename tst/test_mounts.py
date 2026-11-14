#!/usr/bin/env python3
"""Mounts: mount <where> <what> (bin/rootfs/mount.cpp, mount(2)) mounts a
server's directory on a directory of rootfs's: a walk into it, and a call
on its name, go on to that server, which answers the kernel itself.

A directory of /tmp mounted on another, rootfs its server: the names, the
bytes and what is written are the mounted directory's. A mount point is
busy to rmdir and to another mount; umount lets go of it; what is not
there, not a directory, or a device is refused.

File systems on a disk: blk serves the disk, gpt each partition, and
gpt starts /bin/ext4 for one with an ext2 file system, /dev/<part>fs, a
file system to mount, read-only, there before the shell starts. The
disk's ext2 and ext4 file systems are made here (Image): their files'
bytes come whole from runs of blocks, from blocks interleaved with
another file's, through indirect blocks and an extent tree of a leaf,
holes and unwritten extents as zeros; links are followed, an absolute one
from the root of the process; ".." out of the root goes to where it is
mounted, and so does the path of a directory. A file system of features
ext4 does not know is refused, and nothing is written.

    test_mounts.py <vm> <rootfs>
"""
import os
import struct
import subprocess
import sys
import tempfile
import uuid
import zlib

DIR, REG, LNK = 0o040755, 0o100644, 0o120777


class Image:
    """An ext2 file system, or an ext4 one with extents, in one block group,
    as ext4 (bin/rootfs/ext4.cpp) reads it and e2fsck finds it whole: its
    files' blocks taken in turn, or one of every two, the other another's,
    holes left, unwritten extents, fast and slow links."""

    def __init__(self, block, blocks, extents, inodes=64):
        self.block, self.blocks, self.extents, self.inodes = block, blocks, extents, inodes
        self.disk = bytearray(block * blocks)
        self.first_data = 1 if block == 1024 else 0
        self.gdt = self.first_data + 1
        self.block_bitmap, self.inode_bitmap, self.inode_table = self.gdt + 1, self.gdt + 2, self.gdt + 3
        self.next_block = self.inode_table + (inodes * 128 + block - 1) // block
        self.used_blocks = set(range(self.first_data, self.next_block))
        self.next_inode = 11
        self.dirs = 0
        self.root = self.directory(2, 2)
        self.add_dir(self.root, "lost+found")

    def allocate(self):
        while self.next_block in self.used_blocks:
            self.next_block += 1
        if self.next_block >= self.blocks:
            raise ValueError("full")
        self.used_blocks.add(self.next_block)
        return self.next_block

    def directory(self, ino, parent):
        self.dirs += 1
        return {"ino": ino, "parent": parent, "entries": []}

    def inode(self, ino, mode, size, links, blocks_used, flags, i_block):
        raw = struct.pack("<HHIIIIIHHIII", mode, 0, size & 0xFFFFFFFF, 1000000000, 1000000000, 1000000000, 0, 0, links,
                          blocks_used * (self.block // 512), flags, 0)
        raw += i_block.ljust(60, b"\0") + struct.pack("<IIII", 0, 0, size >> 32, 0) + bytes(12)
        assert len(raw) == 128
        at = self.inode_table * self.block + (ino - 1) * 128
        self.disk[at:at + 128] = raw

    def new_inode(self):
        ino = self.next_inode
        self.next_inode += 1
        return ino

    def add_dir(self, parent, name):
        d = self.directory(self.new_inode(), parent["ino"])
        parent["entries"].append((name, d["ino"], 2, d))
        return d

    def put(self, physical, data):
        at = physical * self.block
        self.disk[at:at + len(data)] = data

    def map_blocks(self, blocks):
        """i_block for a list of (logical block, physical block or 0): a
        map of ext2's, indirect blocks taken as the blocks run past the
        direct ones, or ext4's extents, in the inode or a leaf block."""
        if self.extents:
            runs = []
            for logical, physical, unwritten in blocks:
                if physical and runs and runs[-1][0] + runs[-1][1] == logical and runs[-1][2] + runs[-1][1] == physical \
                        and runs[-1][3] == unwritten:
                    runs[-1][1] += 1
                elif physical:
                    runs.append([logical, 1, physical, unwritten])
            extents = b"".join(struct.pack("<IHHI", l, n + (32768 if u else 0), p >> 32, p & 0xFFFFFFFF) for l, n, p, u in runs)
            if len(runs) <= 4:
                return struct.pack("<HHHHI", 0xF30A, len(runs), 4, 0, 0) + extents, 0
            leaf = self.allocate()
            self.put(leaf, struct.pack("<HHHHI", 0xF30A, len(runs), (self.block - 12) // 12, 0, 0) + extents)
            return struct.pack("<HHHHI", 0xF30A, 1, 4, 1, 0) + struct.pack("<IIHH", 0, leaf, 0, 0), 1
        per = self.block // 4
        direct = [0] * 15
        tables, extra = {}, 0

        def table(key, slot):
            nonlocal extra
            if key not in tables:
                tables[key] = [self.allocate(), [0] * per]
                extra += 1
                if slot is not None:
                    slot()
            return tables[key]

        for logical, physical, _ in blocks:
            if not physical:
                continue
            if logical < 12:
                direct[logical] = physical
                continue
            b = logical - 12
            if b < per:
                t = table(("1",), lambda: direct.__setitem__(12, tables[("1",)][0]))
                t[1][b] = physical
                continue
            b -= per
            top = table(("2",), lambda: direct.__setitem__(13, tables[("2",)][0]))
            mid = table(("2", b // per), lambda: top[1].__setitem__(b // per, tables[("2", b // per)][0]))
            mid[1][b % per] = physical
        for number, entries in tables.values():
            self.put(number, struct.pack(f"<{per}I", *entries))
        return struct.pack("<15I", *direct), extra

    def add_file(self, parent, name, data, holes=(), interleave=None, unwritten=(), mode=REG, given=None):
        """A file of data: its blocks taken one by one, or one of every
        two, the other put in interleave, or the blocks given; the blocks in
        holes not taken, those in unwritten taken but unwritten (ext4)."""
        ino = self.new_inode()
        count = (len(data) + self.block - 1) // self.block
        blocks = []
        for logical in range(count):
            if logical in holes:
                blocks.append((logical, 0, False))
                continue
            physical = given[logical] if given is not None else self.allocate()
            if interleave is not None:
                interleave.append(self.allocate())
            chunk = data[logical * self.block:(logical + 1) * self.block]
            self.put(physical, chunk if logical not in unwritten else b"\xAA" * len(chunk))
            blocks.append((logical, physical, logical in unwritten))
        i_block, extra = self.map_blocks(blocks)
        used = sum(1 for _, p, _ in blocks if p) + extra
        self.inode(ino, mode, len(data), 1, used, 0x80000 if self.extents else 0, i_block)
        parent["entries"].append((name, ino, 7 if mode == LNK else 1, None))
        return ino

    def add_link(self, parent, name, target):
        target = target.encode()
        if len(target) < 60:
            ino = self.new_inode()
            self.inode(ino, LNK, len(target), 1, 0, 0, target)
            parent["entries"].append((name, ino, 7, None))
            return ino
        return self.add_file(parent, name, target, mode=LNK)

    def write_dirs(self, d):
        entries = [(".", d["ino"], 2), ("..", d["parent"], 2)] + [(n, i, t) for n, i, t, _ in d["entries"]]
        blocks, current = [], bytearray()
        records = []
        for name, ino, kind in entries:
            raw = name.encode()
            need = (8 + len(raw) + 3) & ~3
            if len(current) + need > self.block:
                records.append(current)
                current = bytearray()
            current += struct.pack("<IHBB", ino, need, len(raw), kind) + raw + bytes(need - 8 - len(raw))
        records.append(current)
        for record in records:
            # The last entry of a block runs to its end.
            at = 0
            last = 0
            while at < len(record):
                last = at
                at += struct.unpack_from("<H", record, at + 4)[0]
            struct.pack_into("<H", record, last + 4, self.block - last)
            physical = self.allocate()
            self.put(physical, bytes(record).ljust(self.block, b"\0"))
            blocks.append((len(blocks), physical, False))
        i_block, extra = self.map_blocks(blocks)
        subdirs = sum(1 for _, _, t, sub in d["entries"] if sub is not None)
        self.inode(d["ino"], DIR, len(blocks) * self.block, 2 + subdirs, len(blocks) + extra, 0x80000 if self.extents else 0, i_block)
        for _, _, _, sub in d["entries"]:
            if sub is not None:
                self.write_dirs(sub)

    def finish(self):
        self.write_dirs(self.root)
        used_inodes = self.next_inode - 1
        bitmap = bytearray(self.block)
        for b in self.used_blocks:
            bitmap[(b - self.first_data) // 8] |= 1 << ((b - self.first_data) % 8)
        for bit in range(self.blocks - self.first_data, self.block * 8):
            bitmap[bit // 8] |= 1 << (bit % 8)
        self.put(self.block_bitmap, bitmap)
        bitmap = bytearray(self.block)
        for i in range(used_inodes):
            bitmap[i // 8] |= 1 << (i % 8)
        for bit in range(self.inodes, self.block * 8):
            bitmap[bit // 8] |= 1 << (bit % 8)
        self.put(self.inode_bitmap, bitmap)
        free_blocks = self.blocks - self.first_data - len(self.used_blocks)
        free_inodes = self.inodes - used_inodes
        self.put(self.gdt, struct.pack("<IIIHHH", self.block_bitmap, self.inode_bitmap, self.inode_table, free_blocks, free_inodes,
                                       self.dirs).ljust(32, b"\0"))
        log = {1024: 0, 2048: 1, 4096: 2}[self.block]
        incompat = 0x2 | (0x40 if self.extents else 0)
        sb = struct.pack("<IIIIIIIIIIIIIHhHHHHIIIIHHIHHIII", self.inodes, self.blocks, 0, free_blocks, free_inodes, self.first_data, log,
                         log, self.block * 8, self.block * 8, self.inodes, 1000000000, 1000000000, 0, -1, 0xEF53, 1, 1, 0,
                         1000000000, 0, 0, 1, 0, 0, 11, 128, 0, 0, incompat, 0)
        self.disk[1024:1024 + len(sb)] = sb
        return bytes(self.disk)


def gpt_image(path, partitions):
    """A disk of 512-byte blocks with a GUID partition table, each partition
    a file system's bytes, one after another from block 2048."""
    first = 2048
    places = []
    for data in partitions:
        places.append((first, data))
        first += len(data) // 512
    sectors = first + 2048
    disk = bytearray(sectors * 512)
    linux = uuid.UUID("0FC63DAF-8483-4772-8E79-3D69D8477DE4").bytes_le
    entries = bytearray(128 * 128)
    for i, (first, data) in enumerate(places):
        entries[i * 128:(i + 1) * 128] = struct.pack("<16s16sQQQ72s", linux, uuid.uuid4().bytes_le, first, first + len(data) // 512 - 1, 0,
                                                     b"")
        disk[first * 512:first * 512 + len(data)] = data
    disk[1024:1024 + len(entries)] = entries
    header = bytearray(struct.pack("<8sIIIIQQQQ16sQIII", b"EFI PART", 0x10000, 92, 0, 0, 1, sectors - 1, 34, sectors - 34,
                                   uuid.uuid4().bytes_le, 2, 128, 128, zlib.crc32(entries)))
    struct.pack_into("<I", header, 16, zlib.crc32(header))
    disk[512:512 + len(header)] = header
    with open(path, "wb") as f:
        f.write(disk)


def tree(image):
    """The files of the test's file systems."""
    root = image.root
    image.add_file(root, "hello.txt", b"hello from ext\n")
    sub = image.add_dir(root, "sub")
    image.add_file(sub, "deep.txt", b"deep\n")
    image.add_link(root, "link", "hello.txt")
    image.add_link(root, "abs", "/etc/motd")
    image.add_link(root, "long", "sub/" + "./" * 40 + "deep.txt")
    image.add_file(root, "big", bytes(i % 251 for i in range(1 << 20)))
    filler = []
    image.add_file(root, "frag", bytes(i * 7 % 256 for i in range(64 * 1024)), interleave=filler)
    image.add_file(root, "filler", b"\x55" * (len(filler) * image.block), given=filler)
    image.add_file(root, "sparse", bytes(1 << 20) + b"end", holes=set(range((1 << 20) // image.block)))
    if image.extents:
        image.add_file(root, "unwritten", b"\x11" * 3 * image.block, unwritten={1})
    return image.finish()


BIND = """\
mkdir /tmp/where /tmp/what && echo inside > /tmp/what/f && mkdir /tmp/what/d
mount /tmp/where /tmp/what && ls /tmp/where && cat /tmp/where/f && ls -d /tmp/where/d
stat -c %i /tmp/where /tmp/what | uniq | wc -l
echo more > /tmp/where/g && cat /tmp/what/g
find /tmp/where | sort
rmdir /tmp/where
mount /tmp/where /tmp/what
umount /tmp/where && ls /tmp/where | wc -l
umount /tmp/where
mount /tmp/missing /tmp/what
mount /tmp/where /tmp/what/f
mount /tmp/where /dev/null
mount /tmp /tmp/what
"""

BIND_WANT = """\
d
f
inside
/tmp/where/d
1
more
/tmp/where
/tmp/where/d
/tmp/where/f
/tmp/where/g
0
"""

BIND_ERRORS = """\
rmdir: '/tmp/where': Device or resource busy
mount: /tmp/what on /tmp/where: Device or resource busy
umount: /tmp/where: Invalid argument
mount: /tmp/what on /tmp/missing: No such file or directory
mount: /tmp/what/f on /tmp/where: Not a directory
mount: /dev/null on /tmp/where: Not a directory
mount: /tmp/what on /tmp: Device or resource busy
"""

EXT = """\
ls /dev | grep fs
mkdir /tmp/two /tmp/four
mount /tmp/two /dev/sda1fs && mount /tmp/four /dev/sda2fs && ls /tmp/four
cat /tmp/two/hello.txt /tmp/four/sub/deep.txt /tmp/two/link /tmp/four/long /tmp/two/long
readlink /tmp/two/abs; head -1 /tmp/four/abs
python3 -c '
import os
for root in ("/tmp/two", "/tmp/four"):
    big = open(root + "/big", "rb").read()
    frag = open(root + "/frag", "rb").read()
    sparse = open(root + "/sparse", "rb").read()
    print(len(big), big == bytes(i % 251 for i in range(1 << 20)), frag == bytes(i * 7 % 256 for i in range(64 * 1024)), len(sparse), sparse[:4], sparse[-3:])
    f = os.open(root + "/big", os.O_RDONLY)
    print(os.pread(f, 6, 5000) == bytes(i % 251 for i in range(5000, 5006)), os.pread(f, 4, 1 << 19), os.pread(f, 10, (1 << 20) - 4), os.pread(f, 10, 1 << 20))
print(open("/tmp/four/unwritten", "rb").read() == b"\\x11" * 4096 + bytes(4096) + b"\\x11" * 4096)
'
cd /tmp/two/sub && pwd -P && ls .. | head -2 && cd /
ls /tmp/four/sub/../..
stat -c '%F %s %h' /tmp/two/sub /tmp/four/big /tmp/two/link
python3 -c 'import os; s = os.statvfs("/tmp/four"); print(s.f_bsize, s.f_flag & 1, s.f_blocks)'
echo x > /tmp/two/new
umount /tmp/two && ls /tmp/two | wc -l
"""

EXT_WANT = """\
sda1fs
sda2fs
abs
big
filler
frag
hello.txt
link
long
lost+found
sparse
sub
unwritten
hello from ext
deep
hello from ext
deep
deep
/etc/motd
pizdos
1048576 True True 1048579 b'\\x00\\x00\\x00\\x00' b'end'
True b'\\xc8\\xc9\\xca\\xcb' b'\\x91\\x92\\x93\\x94' b''
1048576 True True 1048579 b'\\x00\\x00\\x00\\x00' b'end'
True b'\\xc8\\xc9\\xca\\xcb' b'\\x91\\x92\\x93\\x94' b''
True
/tmp/two/sub
abs
big
four
two
directory 1024 2
regular file 1048576 1
symbolic link 9 1
4096 1 2048
0
"""

EXT_ERRORS = """\
ext4: /dev/sda3: features 0x10000 unknown
sh: can't create /tmp/two/new: Read-only file system
"""


def run(vm, rootfs, flags, script, cwd):
    env = {"W2X_MEMORY_TAIL": "strict", "W2X_TAIL_PROFILE": ""}
    return subprocess.run([vm, "run", *flags, "--", rootfs], input=(script + "exit 3\n").encode(), capture_output=True, cwd=cwd, env=env,
                          timeout=180)


def main():
    vm, rootfs = sys.argv[1:3]
    with tempfile.TemporaryDirectory(prefix="pizdos-mounts-") as work:
        empty = os.path.join(work, "empty")
        os.mkdir(empty)
        result = run(vm, rootfs, [], BIND, empty)
        assert (result.returncode, result.stdout.decode(), result.stderr.decode()) == (3, BIND_WANT, BIND_ERRORS), result
        strange = bytearray(Image(1024, 1024, False).finish())
        struct.pack_into("<I", strange, 1024 + 96, 0x2 | 0x10000)
        disk = os.path.join(work, "disk.img")
        gpt_image(disk, [tree(Image(1024, 4096, False)), tree(Image(4096, 2048, True)), bytes(strange)])
        with open(disk, "rb") as f:
            before = f.read()
        result = run(vm, rootfs, ["--disk", f"sda={disk}"], EXT, empty)
        assert (result.returncode, result.stdout.decode(), result.stderr.decode()) == (3, EXT_WANT, EXT_ERRORS), result
        with open(disk, "rb") as f:
            assert f.read() == before
    print("mounts: servers' directories and the ext2 and ext4 file systems of a disk's partitions mount on rootfs's directories")


if __name__ == "__main__":
    main()
