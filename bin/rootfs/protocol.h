// The file protocol of the system: what the kernel speaks for the POSIX
// calls of a process (bin/vm/kernel_remote.go), and what rootfs and the
// services of the system serve (bin/rootfs). An open file is one end of a
// seqpacket pair; its server holds the other and answers on it. Every
// request brings a fresh endpoint, last among its descriptors: the reply
// goes on it, and an OPEN's stays the open file's. A server may hand a
// request on, its descriptors with it, to another server, which answers
// on that endpoint and keeps it: the one that handed it on is out of the
// way from then on. A server that is gone is EIO to the kernel.
//
// A packet is a Header and a body; descriptors ride with it (SCM_RIGHTS).
//
//   Header { op u32, arg u32, uid u32, gid u32 }
//
// uid and gid are who asks: the kernel puts its process's ids, a program
// that speaks the protocol its own. A reply's are zero.
//
//   on a directory's endpoint, about the directory or one name in it: ""
//   is the directory itself; a name is no "." or "..", has no "/" and is
//   never followed: a symbolic link is itself, and to the calls that look
//   at a name the root of a server mounted on it is what it is:
//     RESOLVE   arg handed  root dev u64, root ino u64, links u32, path
//                                         OK step u32, links u32, last u32,
//                                         flags u32, name (resolve); handed
//                                         1: a server handed the walk on,
//                                         the reply's endpoint is kept
//     OPEN      arg flags   mode u32 name OK stat; fds[0] an object: the file is it
//     STAT                  name          OK stat
//     READLINK              name          OK target
//     MKDIR     arg mode    name
//     MKNOD     arg mode    name          a FIFO or a regular file
//     UNLINK    arg flags   name          AT_REMOVEDIR
//     RENAME                dev u64 ino u64 old 0 new   new a name of the
//                                         directory of device dev, inode ino
//     LINK                  dev u64 ino u64 old 0 new
//     SYMLINK               target 0 name
//     CHMOD     arg mode    name
//     CHOWN                 uid u32 gid u32 name
//     UTIMES    arg when    atime u64 mtime u64 name   when: 2 atime now,
//                                         4 mtime now, 8 atime kept,
//                                         16 mtime kept
//     TRUNCATE              size u64 name
//     STATFS                name          OK statfs
//     MOUNT                 name          fds[0] the end of an open of a
//                                         directory of another server: a
//                                         walk into the name, and a call on
//                                         it, go on to that server
//     UNMOUNT               name
//   on a node's endpoint:
//     READ      arg len     off u64 woff u32
//                                         OK count u32; fds[0] a memfd of
//                                         the reader's memory, the bytes
//                                         go there from its byte woff on
//     WRITE     arg append  off u64 len u32 woff u32
//                                         OK count u32 at u64, where an
//                                         append went; fds[0] a memfd of
//                                         the writer's memory, the bytes
//                                         come from there from its byte
//                                         woff on
//     READDIR               after u64     OK (ino u64, pos u64, type u8, name, 0)*
//     FSTAT                               OK stat
//     FCHMOD    arg mode
//     FCHOWN                uid u32 gid u32
//     FUTIMES   arg when    atime u64 mtime u64
//     FTRUNCATE             size u64
//     FSYNC     arg data
//     FSTATFS                             OK statfs
//     FLOCK     arg op                    LOCK_SH, LOCK_EX, LOCK_UN
//     LOCK      arg get     type u32 start u64 len u64
//                                         OK; with get, the lock in the
//                                         way, F_UNLCK for none: type u32
//                                         start u64 len u64
//     PATH                  root dev u64, root ino u64
//                                         OK the path from the root of the
//                                         process: a directory's, a file's
//                                         by the name it was opened by
//   on the endpoint of rootfs's /dev/init:
//     REGISTER  arg mode    name          OK stat: a device, S_IFCHR or
//                                         S_IFBLK, or a file system to
//                                         mount, S_IFSOCK, of that name in
//                                         /dev.
//                                         fds[0] is the end of a pair whose
//                                         other end the service keeps:
//                                         rootfs hands every OPEN of the
//                                         name on to it, and the name goes
//                                         when the end closes
//   on the root's endpoint of a server rootfs mounts, with no reply:
//     MOUNTED                             fds[0] an endpoint of the mount
//                                         point: a walk out of the server's
//                                         root by ".." goes on there as
//                                         "../" and the names left, and a
//                                         PATH there is where it is mounted
//   replies: OK, ERR with arg the errno
//   stat { ino u64, size u64, mtime u64 nanoseconds, mode u32, nlink u32,
//          uid u32, gid u32, atime u64, dev u64, ctime u64 }
//   statfs { blocks u64, bfree u64, files u64, ffree u64, dev u64,
//            bsize u32, namemax u32, flags u32 }  flags: 1 read-only

#pragma once

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>

enum : uint32_t {
    opOpen = 1, opStat, opReadlink, opMkdir, opUnlink, opRename, opSymlink,
    opResolve, opRead, opWrite, opReaddir, opFstat, opChmod, opChown, opUtimes,
    opTruncate, opFchmod, opFchown, opFutimes, opFtruncate, opFsync,
    opMknod, opLink, opStatfs, opFstatfs, opFlock, opLock, opPath, opOk, opErr,
    opRegister, opMount, opUnmount, opMounted,
};

// The most descriptors a packet brings: a READ's or WRITE's memfd and its
// endpoint, a REGISTER's end and its endpoint.
constexpr size_t maxFds = 4;
constexpr uint32_t noId = 0xffffffff;  // a uid or gid CHOWN keeps
// UTIMES's when.
constexpr uint32_t whenAtimeNow = 2;
constexpr uint32_t whenMtimeNow = 4;
constexpr uint32_t whenAtimeKept = 8;
constexpr uint32_t whenMtimeKept = 16;
// RESOLVE's reply: where the walk ended, the kind of the last name and
// what goes with it.
constexpr uint32_t stepDone = 0;  // at the directory of the last name
constexpr uint32_t stepRoot = 1;  // the walk goes on from the root of the process
constexpr uint32_t lastName = 0;
constexpr uint32_t lastDot = 1;
constexpr uint32_t lastDotdot = 2;
constexpr uint32_t lastNone = 3;  // the path is "/" alone
constexpr uint32_t stepSlash = 1;  // a slash ends the path
constexpr uint32_t stepKept = 2;  // the reply's endpoint is the directory's from now on
constexpr uint32_t resolveHanded = 1;  // RESOLVE's arg: a server handed the walk on

struct Header {
    uint32_t op;
    uint32_t arg;
    uint32_t uid;
    uint32_t gid;
};

struct Stat {
    uint64_t ino;
    uint64_t size;
    uint64_t mtime;
    uint32_t mode;
    uint32_t nlink;
    uint32_t uid;
    uint32_t gid;
    uint64_t atime;
    uint64_t dev;
    uint64_t ctime;
};

struct [[gnu::packed]] Statfs {
    uint64_t blocks;
    uint64_t bfree;
    uint64_t files;
    uint64_t ffree;
    uint64_t dev;
    uint32_t bsize;
    uint32_t namemax;
    uint32_t flags;
};

static_assert(sizeof(Header) == 16);
static_assert(sizeof(Stat) == 64 && offsetof(Stat, atime) == 40);
static_assert(sizeof(Statfs) == 52);

// Sends the parts as one packet, the descriptors with it: what was sent,
// or -1 and errno. An endpoint nobody reads any more is EPIPE, no signal.
inline ssize_t sendPacket(int fd, const iovec* parts, size_t count, const int* fds, size_t fdCount, int flags = 0) {
    alignas(cmsghdr) unsigned char control[CMSG_SPACE(maxFds * sizeof(int))] = {};
    msghdr m{};
    m.msg_iov = const_cast<iovec*>(parts);
    m.msg_iovlen = int(count);
    if (fdCount > 0) {
        if (fdCount > maxFds) {
            errno = EINVAL;
            return -1;
        }
        m.msg_control = control;
        m.msg_controllen = socklen_t(CMSG_LEN(fdCount * sizeof(int)));
        cmsghdr* c = CMSG_FIRSTHDR(&m);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = m.msg_controllen;
        memcpy(CMSG_DATA(c), fds, fdCount * sizeof(int));
    }
    return sendmsg(fd, &m, flags | MSG_NOSIGNAL);
}

// Receives one packet into buffer, its descriptors into fds, close on
// exec: its length, 0 at the end of the endpoint, or -1 and errno. The
// kernel drops descriptors past maxFds.
inline ssize_t receivePacket(int fd, void* buffer, size_t size, int* fds, size_t& fdCount, int flags = 0) {
    alignas(cmsghdr) unsigned char control[CMSG_SPACE(maxFds * sizeof(int))];
    iovec iov{buffer, size};
    msghdr m{};
    m.msg_iov = &iov;
    m.msg_iovlen = 1;
    m.msg_control = control;
    m.msg_controllen = sizeof control;
    fdCount = 0;
    const ssize_t n = recvmsg(fd, &m, flags | MSG_CMSG_CLOEXEC);
    if (n < 0) {
        return n;
    }
    for (cmsghdr* c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS && c->cmsg_len >= CMSG_LEN(0)) {
            const size_t count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            for (size_t i = 0; i < count && fdCount < maxFds; ++i) {
                memcpy(&fds[fdCount++], CMSG_DATA(c) + i * sizeof(int), sizeof(int));
            }
        }
    }
    return n;
}
