#!/usr/bin/env python3
"""Disks of the host as block devices of the system: vm run --disk gives
rootfs a file of the host, /bin/blk serves it as /dev/<name> and starts
/bin/gpt for each partition of its GUID partition table, /dev/<name><n>,
or <name>p<n> after a digit; the shell starts once they are all there. A
partition's READs and WRITEs go on to the disk, moved and cut at the
partition's end, and the disk's bytes go between the host's file and the
reader's or writer's memory. What is written is in the host's file after
vm ends; a disk with no partition table is a disk alone.

    test_disks.py <vm> <rootfs>
"""
import os
import struct
import subprocess
import sys
import tempfile
import uuid
import zlib

MIB = 1 << 20


def gpt_image(path, sectors, partitions):
    """A disk of 512-byte blocks with a GUID partition table: each partition,
    (first block, last block, bytes), has its bytes at its start."""
    disk = bytearray(sectors * 512)
    disk[446:462] = struct.pack("<BBBBBBBBII", 0, 0, 2, 0, 0xEE, 0xFF, 0xFF, 0xFF, 1, sectors - 1)
    disk[510:512] = b"\x55\xaa"
    linux = uuid.UUID("0FC63DAF-8483-4772-8E79-3D69D8477DE4").bytes_le
    entries = bytearray(128 * 128)
    for i, (first, last, data) in enumerate(partitions):
        entries[i * 128:(i + 1) * 128] = struct.pack("<16s16sQQQ72s", linux, uuid.uuid4().bytes_le, first, last, 0,
                                                     f"part{i + 1}".encode("utf-16-le"))
        disk[first * 512:first * 512 + len(data)] = data
    disk[1024:1024 + len(entries)] = entries
    header = bytearray(struct.pack("<8sIIIIQQQQ16sQIII", b"EFI PART", 0x10000, 92, 0, 0, 1, sectors - 1, 34, sectors - 34,
                                   uuid.uuid4().bytes_le, 2, 128, 128, zlib.crc32(entries)))
    struct.pack_into("<I", header, 16, zlib.crc32(header))
    disk[512:512 + len(header)] = header
    with open(path, "wb") as f:
        f.write(disk)


SCRIPT = """\
ls /dev
stat -c '%F %a' /dev/sda /dev/sda1 /dev/vd0p1
head -c 13 /dev/sda1; echo; head -c 13 /dev/sda2; echo; head -c 5 /dev/vd0p1; echo
python3 -c '
import os
d = os.open("/dev/sda", os.O_RDONLY)
p = os.open("/dev/sda1", os.O_RDWR)
print(os.lseek(d, 0, 2), os.lseek(p, 0, 2), os.fstat(p).st_size, os.lseek(os.open("/dev/raw", os.O_RDONLY), 0, 2))
print(os.pread(p, 10, (1 << 20) - 4), os.pread(p, 4, 1 << 20))
pattern = bytes(i % 251 for i in range(1 << 20))
print(os.pwrite(p, pattern, 0), os.pread(p, 1 << 20, 0) == pattern, os.pread(d, 1 << 20, 2048 * 512) == pattern)
print(os.pwrite(p, b"written", 100), os.pread(d, 7, 2048 * 512 + 100))
print(os.pwrite(p, b"edge", (1 << 20) - 2), os.pread(os.open("/dev/sda2", os.O_RDONLY), 4, 0))
try: os.pwrite(p, b"x", 1 << 20)
except OSError as e: print(e.strerror)
os.lseek(p, 50, 0)
os.write(p, b"ab")
print(os.lseek(p, 0, 1))
os.fsync(p)
'
"""

WANT = """\
init
null
random
raw
sda
sda1
sda2
tty
urandom
vd0
vd0p1
zero
block special file 660
block special file 660
block special file 660
partition one
partition two
third
4194304 1048576 1048576 1048576
b'\\x00\\x00\\x00\\x00' b''
1048576 True True
7 b'written'
2 b'part'
No space left on device
52
"""


def main():
    vm, rootfs = sys.argv[1:3]
    with tempfile.TemporaryDirectory(prefix="pizdos-disks-") as work:
        sda, vd0, raw = (os.path.join(work, name) for name in ("sda.img", "vd0.img", "raw.img"))
        gpt_image(sda, 8192, [(2048, 4095, b"partition one"), (4096, 6143, b"partition two")])
        gpt_image(vd0, 4096, [(2048, 3071, b"third")])
        with open(raw, "wb") as f:
            f.write(bytes(MIB))
        empty = os.path.join(work, "empty")
        os.mkdir(empty)
        env = {"W2X_MEMORY_TAIL": "strict", "W2X_TAIL_PROFILE": ""}
        result = subprocess.run([vm, "run", "--disk", f"sda={sda}", "--disk", f"vd0={vd0}", "--disk", f"raw={raw}", "--", rootfs],
                                input=(SCRIPT + "exit 3\n").encode(), capture_output=True, cwd=empty, env=env, timeout=180)
        assert (result.returncode, result.stdout.decode(), result.stderr) == (3, WANT, b""), result
        with open(sda, "rb") as f:
            disk = f.read()
        want = bytearray(i % 251 for i in range(MIB))
        want[50:52] = b"ab"
        want[100:107] = b"written"
        want[MIB - 2:] = b"ed"
        assert disk[2048 * 512:4096 * 512] == want
        assert disk[4096 * 512:4096 * 512 + 13] == b"partition two"
    print("disks: vm's disks are block devices of the system, their partitions too, served by services, written through to the host")


if __name__ == "__main__":
    main()
