# File systems

The design of pizd0s's file systems: what is decided, what is proposed and
what is open.

## Principles

- pizd0s is not Linux and does not emulate Linux's file system namespace:
  no `/dev`, no `/proc`, no Linux `/sys`, no device nodes. A program sees
  the files of file systems and one tree the kernel provides itself.
- That tree is `/sys`: `/sys/dev`, the devices, and `/sys/pid`, the
  processes, our procfs to come. Nothing else of the system has a path.
- Every other file belongs to a file system, and a file system is a
  process.

## A file system is a process

- A file system is an ordinary program, a wasm module wasm2o compiles,
  linked with lib/c. It has the kernel's API like any program and its
  block device as a descriptor, a file of `/sys/dev` it reads and writes
  with `pread`, `pwrite` and `fdatasync`. It serves the file system to the
  kernel.
- The protocol is FUSE's: the requests and replies of `fuse_kernel.h`
  (lookup, getattr, setattr, readlink, symlink, mknod, mkdir, unlink,
  rmdir, rename, link, open, read, write, statfs, release, fsync, readdir,
  create and the rest), versioned and documented, with servers that exist.
  A system call here is a function call, so a request costs the server two
  calls: one reads it from its channel, one writes the reply.
- Only the protocol is taken from Linux. There is no `/dev/fuse` and no
  Linux `mount(2)` with its option string: how a server gets its channel
  and where it is mounted are our own calls (open).
- A server that ends ends its mount: calls on its files fail. The kernel
  keeps no state of a file system but its mount table and the handles of
  open files.
- A server with one thread takes requests in order; the kernel queues the
  rest, and the processes that wait block in their goroutines.

## The kernel

- A mount table: a path is the host directory the kernel runs in (today's
  `--root`), a server's channel, or `/sys`.
- The kernel walks paths itself, a component at a time, across mounts: in
  a host mount through `openat2` with `RESOLVE_IN_ROOT` as now, in a
  server's by lookup requests; it follows symlinks with readlink requests
  and takes `..` across mount points.
- A descriptor of a server's file reads, writes, seeks, stats, lists,
  syncs and truncates by requests; `mmap` reads it in, as it does a host
  file now.
- The host root stays the first mount and servers mount over its
  directories. Later the root itself can be a server's mount, the host
  root only what the system starts from.

## Threads

- The first file systems need none: a server loops on its channel.
- ZFS needs them: its taskqs, the txg sync thread, `zio_wait` waiting on
  a condition another thread signals. The proposal is green threads in the
  kernel: each pthread a goroutine in the same instance, one running at a
  time, switching where a thread blocks (mutexes, conditions, sleep, I/O),
  the kernel keeping each thread's stack pointer global and errno. That
  needs no wasm atomics and no shared memory. Nothing is preempted until
  the compiler has atomic instructions: a thread switched in the middle of
  a read-modify-write would break it. Python and LLVM would have threads
  too.

## Which file systems

- First, ext4 by lwext4 through fuse-lwext4: a journal with transactions
  and recovery, extents, directory indexes, xattrs, symlinks and hard
  links, needing libc and malloc only, no threads. Two of its files make it
  GPLv2 as a whole; that binds the server only, since the kernel speaks
  the protocol and links nothing of it.
- fuse2fs of e2fsprogs is the alternative, and e2fsprogs gives the tools
  (`mke2fs`, `e2fsck`, `debugfs`); it runs with one thread (`-s`) but
  writes no journal.
- Read-only images, for the system's own files: erofs (erofsfuse,
  `mkfs.erofs`) or squashfs (squashfuse), both with one thread.
- ZFS after threads. OpenZFS's core (spa, dmu, zio, arc, zil, zap) builds
  in userland already, as libzpool for ztest and zdb, over a pthreads SPL,
  and every SIMD routine has a scalar one. Its POSIX layer is each OS's
  own, so ours, from the protocol to its znodes, is the port's work; the
  ARC is capped below wasm32's 4 GiB.
- Not FatFs or littlefs: no Unix semantics.

## The first test

An ext4 image made by `mkfs.ext4` on the host is a disk of `/sys/dev`; the
lwext4 server mounts it; busybox writes and reads files through the mount;
after the run the host's `e2fsck` checks the image.

## Open

- The calls: how a server gets its channel and mounts it, and who may.
- The names of `/sys/dev`: disks given to wasm2o (a host file or device
  each), terminals, null and the like.
- What `/sys/pid` holds.
- The host devices the kernel passes through today (`/dev/null` and its
  kin) and the programs that name them (busybox, Python's `os.devnull`):
  they move to `/sys/dev`, and lib/c's `paths.h` and those programs follow.
