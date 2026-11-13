// rootfs: the system, the first process. It serves three file systems
// from one loop: the image linked into it, read-only, at the root; /dev,
// the terminal the kernel gave vm as descriptor 0, null, zero, urandom and
// random; and /tmp, a tree in its memory of at most tmpCapacity bytes, a
// quarter of what a memory of wasm32 holds. It makes itself the root of
// everything else with what POSIX has: one seqpacket pair, chroot to one
// end in the shell's process. It starts the shell on the terminal with a
// clean environment, reaps what ends under it, and ends with the shell's
// status, as a system does.
//
// A file server is a tree of nodes behind endpoints. An open file is one
// end of a seqpacket pair; the server holds the other and answers on it.
// Every request brings a fresh endpoint, last among its descriptors: the
// reply goes on it, and an OPEN's stays the open file's. The kernel
// speaks this protocol for the POSIX calls of a process
// (bin/vm/kernel_remote.go) and walks a path as Linux does: RESOLVE walks
// it here to the directory of its last name, and the call is one on that
// name, which the kernel follows itself when it is a symbolic link. /dev
// and /tmp are servers of this process mounted on names of the image
// (mountOn): a walk goes into the root of a server mounted on a name, and
// by ".." out of that root to where it is mounted.
//
// A packet is a Header and a body; descriptors ride with it (SCM_RIGHTS).
//
//   Header { op u32, arg u32, uid u32, gid u32 }
//
// uid and gid are who asks: the kernel puts its process's ids, a program
// that speaks the protocol its own. A server makes what it creates
// theirs and lets them do what its modes let them: root everything but
// executing what nobody may, the owner what the owner's bits say, the
// group of gid the group's, everyone else the others'. A path is searched
// through directories who asks may search, a name is made or removed in a
// directory they may write, and in a sticky one removed only by the
// owner of the name's node or of the directory. A reply's ids are zero.
//
//   on a directory's endpoint, about the directory or one name in it: ""
//   is the directory itself; a name is no "." or "..", has no "/" and is
//   never followed: a symbolic link is itself, and to the calls that look
//   at a name the root of a server mounted on it is what it is:
//     RESOLVE               root dev u64, root ino u64, links u32, path
//                                         OK step u32, links u32, last u32,
//                                         flags u32, name (resolve)
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
//   on a node's endpoint:
//     READ      arg len     off u64       OK data
//     WRITE     arg append  off u64 data  OK count u32 at u64
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
//   replies: OK, ERR with arg the errno
//   stat { ino u64, size u64, mtime u64 nanoseconds, mode u32, nlink u32,
//          uid u32, gid u32, atime u64, dev u64, ctime u64 }
//   statfs { blocks u64, bfree u64, files u64, ffree u64, dev u64,
//            bsize u32, namemax u32, flags u32 }  flags: 1 read-only
//
// The root of the process that asks is a device and an inode: ".." stays
// there, and an absolute link goes on from there. A node of this
// process's servers is found by them; a root that is no node of theirs
// sends the walk back to the kernel to go on from there.
//
// A directory's entries have positions, each after those before it: a
// READDIR gives the entries after one, so a reading that removes what it
// read misses nothing. A node has the names that are links to it, a
// directory one. dev is the server's: the number of a pair it makes as it
// makes its root, which no other pair in the kernel has.
//
// A WRITE with append writes at the end the file has when the server takes
// it, which no other write comes between. A lock is an endpoint's, an open
// file's, as Linux's open file description locks are: flock's on the
// whole file, fcntl's on a range of its bytes, refused with EAGAIN when
// another endpoint holds one in the way, and gone when the endpoint
// closes. A FIFO is a node and nothing more: the kernel makes the pipe its
// openers share. A writable server holds what its capacity lets it, in
// bytes and in nodes, a node a block's worth: past that, ENOSPC.
//
// Memory. A request is received into one buffer, packet, and answered
// before the next: its names and data are read where they are in it. A
// reply is sent from where its parts are: a header on the stack, a stat
// built there, a name of the request, a file's bytes, a link's target.
// What a reply assembles, the entries of a directory, a path, random
// bytes, goes into one buffer of its own, out. Nothing is allocated to
// answer a request; what a request makes is: a node, its name, a file's
// bytes, an endpoint. The image's files and names are its own bytes,
// never copied. The names of every directory are one open-addressing
// table, by directory and name; a directory keeps its entries in the
// order of their positions.

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

using u8 = __UINT8_TYPE__;
using u32 = __UINT32_TYPE__;
using u64 = __UINT64_TYPE__;
using i32 = __INT32_TYPE__;
using i64 = __INT64_TYPE__;
using usize = __SIZE_TYPE__;
using uptr = __UINTPTR_TYPE__;

// Placement new expressions are lowered directly by Clang; no runtime
// call uses this overload. Its declaration supplies the freestanding ABI.
void* operator new(usize, void*) noexcept;

// The image: a ustar archive (bin/rootfs/image.py) linked in as data
// (bin/rootfs/make.py).
extern "C" const u8 rootfs_image[];
extern "C" const u32 rootfs_image_size;

namespace {
    static constexpr u32 atRemovedir = 0x200;
    static constexpr u32 oCreat = 0100;
    static constexpr u32 oExcl = 0200;
    static constexpr u32 oTrunc = 01000;
    static constexpr u32 oNofollow = 0400000;
    static constexpr u32 oDirectory = 0200000;
    static constexpr u32 oPath = 010000000;
    static constexpr u32 whenAtimeNow = 2;
    static constexpr u32 whenMtimeNow = 4;
    static constexpr u32 whenAtimeKept = 8;
    static constexpr u32 whenMtimeKept = 16;
    static constexpr int msgCmsgCloexec = 0x40000000;
    static constexpr usize maxFds = 4;
    static constexpr usize maxPacket = 65536;
    static constexpr u64 blockSize = 4096;
    static constexpr u8 dtFifo = 1;
    static constexpr u8 dtChr = 2;
    static constexpr u8 dtDir = 4;
    static constexpr u8 dtReg = 8;
    static constexpr u8 dtLnk = 10;
    static constexpr u32 sIfmt = 0170000;
    static constexpr u32 sIfifo = 0010000;
    static constexpr u32 sIfchr = 0020000;
    static constexpr u32 sIfdir = 0040000;
    static constexpr u32 sIfreg = 0100000;
    static constexpr u32 sIflnk = 0120000;
    static constexpr u32 sSticky = 01000;
    static constexpr u32 lockSh = 1;
    static constexpr u32 lockEx = 2;
    static constexpr u32 lockNb = 4;
    static constexpr u32 lockUn = 8;
    static constexpr u32 fRdlck = 0;
    static constexpr u32 fWrlck = 1;
    static constexpr u32 fUnlck = 2;
    static constexpr u32 stRdonly = 1;
    static constexpr u32 noId = 0xffffffff;
    static constexpr u64 past = ~u64(0);  // the stop of a lock to the end of a file and past it
    static constexpr usize nameMax = 255;  // the longest name
    static constexpr u32 maxLinks = 40;  // the symbolic links one path goes through, Linux's
    static constexpr u64 offsetMax = 0x7fffffffffffffff;  // past it, an offset no file has
    static constexpr u64 tmpCapacity = u64(1) << 30;
    // RESOLVE's reply: where the walk ended, the kind of the last name and
    // what goes with it.
    static constexpr u32 stepDone = 0;  // at the directory of the last name
    static constexpr u32 stepRoot = 1;  // the walk goes on from the root of the process
    static constexpr u32 lastName = 0;
    static constexpr u32 lastDot = 1;
    static constexpr u32 lastDotdot = 2;
    static constexpr u32 lastNone = 3;  // the path is "/" alone
    static constexpr u32 stepSlash = 1;  // a slash ends the path
    static constexpr u32 stepKept = 2;  // the reply's endpoint is the directory's from now on

    enum : u32 {
        opOpen = 1, opStat, opReadlink, opMkdir, opUnlink, opRename, opSymlink,
        opResolve, opRead, opWrite, opReaddir, opFstat, opChmod, opChown, opUtimes,
        opTruncate, opFchmod, opFchown, opFutimes, opFtruncate, opFsync,
        opMknod, opLink, opStatfs, opFstatfs, opFlock, opLock, opPath, opOk, opErr,
    };

    struct Header {
        u32 op;
        u32 arg;
        u32 uid;
        u32 gid;
    };

    struct Stat {
        u64 ino;
        u64 size;
        u64 mtime;
        u32 mode;
        u32 nlink;
        u32 uid;
        u32 gid;
        u64 atime;
        u64 dev;
        u64 ctime;
    };

    struct __attribute__((packed)) Statfs {
        u64 blocks;
        u64 bfree;
        u64 files;
        u64 ffree;
        u64 dev;
        u32 bsize;
        u32 namemax;
        u32 flags;
    };

    static_assert(sizeof(Header) == 16);
    static_assert(sizeof(Stat) == 64 && __builtin_offsetof(Stat, atime) == 40);
    static_assert(sizeof(Statfs) == 52);

    static usize length(const char* text) {
        usize n = 0;
        while (text[n]) {
            ++n;
        }
        return n;
    }

    static void say(const char* text) {
        (void)write(2, text, length(text));
    }

    [[noreturn]] static void fatal(const char* why, const char* what = "") {
        say(why);
        say(what);
        say("\n");
        exit(1);
    }

    static void sayNumber(int value) {
        char digits[16];
        usize at = sizeof digits;
        u32 v = value < 0 ? 0u - u32(value) : u32(value);
        do {
            digits[--at] = char('0' + v % 10);
            v /= 10;
        } while (v);
        if (value < 0) {
            digits[--at] = '-';
        }
        (void)write(2, digits + at, sizeof digits - at);
    }

    static usize checkedAdd(usize a, usize b) {
        if (b > ~usize(0) - a) {
            fatal("rootfs: size overflow");
        }
        return a + b;
    }

    static usize checkedMultiply(usize a, usize b) {
        if (a != 0 && b > ~usize(0) / a) {
            fatal("rootfs: size overflow");
        }
        return a * b;
    }

    static void* allocate(usize size) {
        void* p = malloc(size);
        if (!p) {
            fatal("rootfs: out of memory");
        }
        return p;
    }

    static void* reallocate(void* p, usize size) {
        void* q = realloc(p, size);
        if (!q) {
            fatal("rootfs: out of memory");
        }
        return q;
    }

    template <typename T>
    static T least(T a, T b) {
        return a < b ? a : b;
    }

    // ---- containers, after bin/aot/aot.cpp's ---------------------------------

    static constexpr usize objectAlignment = 16;

    struct Buffer {
        struct alignas(objectAlignment) Header {
            usize used;
            usize allocation;
        };

        constexpr Buffer();
        ~Buffer();
        Buffer(const Buffer&) = delete;
        Buffer& operator=(const Buffer&) = delete;
        void* data();
        const void* data() const;
        usize size() const;
        usize capacity() const;
        void reserve(usize size);
        void grow(usize size);
        void resize(usize size);
        void resizeZeroed(usize size);
        void append(const void* data, usize size);
        void clear();
        void swap(Buffer& other);

        Header* header() const;
        void* storage;
    };

    alignas(objectAlignment) constinit const Buffer::Header emptyBuffer{};

    constexpr Buffer::Buffer()
        : storage(const_cast<Header*>(&emptyBuffer) + 1)
    {
    }
}

Buffer::~Buffer() {
    if (header() != &emptyBuffer) {
        free(header());
    }
}

Buffer::Header* Buffer::header() const {
    return static_cast<Header*>(storage) - 1;
}

void* Buffer::data() {
    return storage;
}

const void* Buffer::data() const {
    return storage;
}

usize Buffer::size() const {
    return header()->used;
}

usize Buffer::capacity() const {
    return header()->allocation == 0 ? 0 : header()->allocation - sizeof(Header);
}

inline void Buffer::reserve(usize size) {
    if (size > capacity()) {
        grow(size);
    }
}

// Growth is by half again of what there is, from 64 bytes: a file
// written to its end in pieces is copied a few times, and holds at most
// half of what it is more.
__attribute__((noinline)) void Buffer::grow(usize size) {
    auto* before = header();
    u64 bytes = u64(checkedAdd(size, sizeof(Header)));
    if (before != &emptyBuffer && bytes < u64(before->allocation) * 3 / 2) {
        bytes = u64(before->allocation) * 3 / 2;
    }
    bytes = bytes < 64 ? 64 : (bytes + objectAlignment - 1) & ~u64(objectAlignment - 1);
    if (bytes > ~usize(0)) {
        fatal("rootfs: size overflow");
    }
    Header* next;
    if (before == &emptyBuffer) {
        next = static_cast<Header*>(allocate(usize(bytes)));
        next->used = 0;
    } else {
        next = static_cast<Header*>(reallocate(before, usize(bytes)));
    }
    next->allocation = usize(bytes);
    storage = next + 1;
}

inline void Buffer::resize(usize size) {
    reserve(size);
    if (size != 0 || header() != &emptyBuffer) {
        header()->used = size;
    }
}

// A size set, the bytes past what there was zeros; nothing holds no
// memory.
void Buffer::resizeZeroed(usize size) {
    const usize before = this->size();
    if (size == 0) {
        clear();
        return;
    }
    reserve(size);
    if (size > before) {
        memset(static_cast<u8*>(storage) + before, 0, size - before);
    }
    header()->used = size;
}

void Buffer::append(const void* data, usize size) {
    if (size == 0) {
        return;
    }
    const usize before = this->size();
    reserve(checkedAdd(before, size));
    memcpy(static_cast<u8*>(storage) + before, data, size);
    header()->used = before + size;
}

void Buffer::clear() {
    if (header() != &emptyBuffer) {
        free(header());
    }
    storage = const_cast<Header*>(&emptyBuffer) + 1;
}

void Buffer::swap(Buffer& other) {
    auto* temporary = storage;
    storage = other.storage;
    other.storage = temporary;
}

namespace {
    template <typename T>
    struct SmallVec {
        static_assert(__is_trivially_copyable(T));
        static_assert(__is_trivially_destructible(T));
        static_assert(alignof(T) <= objectAlignment);

        constexpr SmallVec() = default;
        SmallVec(const SmallVec&) = delete;
        SmallVec& operator=(const SmallVec&) = delete;

        T* data() {
            return static_cast<T*>(buffer.data());
        }

        const T* data() const {
            return static_cast<const T*>(buffer.data());
        }

        usize size() const {
            return buffer.size() / sizeof(T);
        }

        bool empty() const {
            return size() == 0;
        }

        T& operator[](usize index) {
            return data()[index];
        }

        const T& operator[](usize index) const {
            return data()[index];
        }

        void resize(usize count) {
            buffer.resize(checkedMultiply(count, sizeof(T)));
        }

        void clear() {
            buffer.resize(0);
        }

        void pushBack(T value) {
            const usize at = size();
            resize(checkedAdd(at, 1));
            data()[at] = value;
        }

        // Keeps the elements keep takes, in their order.
        template <typename F>
        void keepIf(F keep) {
            usize kept = 0;
            for (usize i = 0; i < size(); ++i) {
                if (keep(data()[i])) {
                    data()[kept++] = data()[i];
                }
            }
            resize(kept);
        }

        Buffer buffer;
    };

    // An open-addressing table of pointers, linear probing, removal by
    // shifting back what follows: each slot keeps the hash of its item,
    // which a probe compares first.
    template <typename T>
    struct OpenTable {
        struct Slot {
            u32 hash;
            T* item;  // none: the slot is empty
        };

        template <typename Same>
        T* find(u32 hash, Same same) const {
            if (!slots) {
                return nullptr;
            }
            for (u32 i = hash & mask;; i = (i + 1) & mask) {
                const Slot& slot = slots[i];
                if (!slot.item) {
                    return nullptr;
                }
                if (slot.hash == hash && same(slot.item)) {
                    return slot.item;
                }
            }
        }

        void insert(u32 hash, T* item) {
            if (!slots || (count + 1) * 2 > mask + 1) {
                grow();
            }
            put(hash, item);
            ++count;
        }

        void erase(u32 hash, const T* item) {
            u32 i = hash & mask;
            while (slots[i].item != item) {
                if (!slots[i].item) {
                    fatal("rootfs: no such item in a table");
                }
                i = (i + 1) & mask;
            }
            for (u32 j = i;;) {
                j = (j + 1) & mask;
                if (!slots[j].item) {
                    break;
                }
                const u32 home = slots[j].hash & mask;
                const bool stays = i <= j ? i < home && home <= j : i < home || home <= j;
                if (!stays) {
                    slots[i] = slots[j];
                    i = j;
                }
            }
            slots[i] = {0, nullptr};
            --count;
        }

        void put(u32 hash, T* item) {
            u32 i = hash & mask;
            while (slots[i].item) {
                i = (i + 1) & mask;
            }
            slots[i] = {hash, item};
        }

        void grow() {
            Slot* old = slots;
            const usize before = old ? usize(mask) + 1 : 0;
            const usize capacity = old ? checkedMultiply(before, 2) : 16;
            const usize bytes = checkedMultiply(capacity, sizeof(Slot));
            slots = static_cast<Slot*>(allocate(bytes));
            memset(slots, 0, bytes);
            mask = u32(capacity - 1);
            for (usize i = 0; i < before; ++i) {
                if (old[i].item) {
                    put(old[i].hash, old[i].item);
                }
            }
            free(old);
        }

        Slot* slots = nullptr;
        u32 mask = 0;
        u32 count = 0;
    };

    static u32 finish(u64 h) {
        h ^= h >> 33;
        h *= 0xff51afd7ed558ccdull;
        h ^= h >> 33;
        h *= 0xc4ceb9fe1a85ec53ull;
        h ^= h >> 33;
        return u32(h);
    }

    // ---- names and bodies ------------------------------------------------------

    // Bytes of a packet, of the image or of a node, where they are.
    struct Name {
        const u8* at = nullptr;
        usize length = 0;

        bool is(const char* text) const {
            usize i = 0;
            for (; text[i]; ++i) {
                if (i >= length || at[i] != u8(text[i])) {
                    return false;
                }
            }
            return i == length;
        }

        bool has(u8 c) const {
            for (usize i = 0; i < length; ++i) {
                if (at[i] == c) {
                    return true;
                }
            }
            return false;
        }
    };

    static Name literal(const char* text) {
        return {reinterpret_cast<const u8*>(text), length(text)};
    }

    // A request's body, read from its start.
    struct Body {
        const u8* at;
        const u8* end;

        usize size() const {
            return usize(end - at);
        }

        u32 word() {
            if (size() < 4) {
                fatal("a short packet");
            }
            u32 v;
            __builtin_memcpy(&v, at, 4);
            at += 4;
            return v;
        }

        u64 quad() {
            if (size() < 8) {
                fatal("a short packet");
            }
            u64 v;
            __builtin_memcpy(&v, at, 8);
            at += 8;
            return v;
        }

        // A string up to its NUL, or the end, and past the NUL.
        Name string() {
            const u8* start = at;
            while (at < end && *at != 0) {
                ++at;
            }
            const Name name{start, usize(at - start)};
            if (at < end) {
                ++at;
            }
            return name;
        }

        Name rest() {
            const Name name{at, size()};
            at = end;
            return name;
        }
    };

    // ---- the tree ----------------------------------------------------------------

    struct Node;
    struct Server;

    enum class Kind : u8 {
        Dir,
        File,
        Link,
        Fifo,  // a named pipe: the kernel makes the pipe
        Object,  // an object of the kernel the server hands out: a tty, a disk
        Null,  // reads nothing, takes everything
        Zero,  // reads zeros
        Random,  // reads the kernel's random bytes
    };

    // A lock of fcntl: an endpoint's, on the bytes [start, stop), to read
    // or to write.
    struct Range {
        i32 owner;
        bool write;
        u64 start;
        u64 stop;
    };

    // A name of a directory: its bytes are the image's or follow the entry.
    struct Entry {
        Node* dir;
        Node* node;
        u64 pos;  // where the name is among the directory's entries
        const u8* name;
        u32 length;
        u32 hash;
    };

    // A directory's entries in the order of their positions; a removed
    // one leaves a hole, until holes are half of them.
    struct Slot {
        u64 pos;
        Entry* entry;  // none: removed
    };

    // A file's bytes or a link's target: the image's, or the node's own.
    struct Content {
        const u8* view = nullptr;
        usize viewSize = 0;
        Buffer own;

        const u8* data() const {
            return view ? view : static_cast<const u8*>(own.data());
        }

        usize size() const {
            return view ? viewSize : own.size();
        }
    };

    struct Node {
        u64 ino = 0;
        u32 perm = 0;  // the permission bits
        u32 uid = 0;
        u32 gid = 0;
        u64 atime = 0;  // nanoseconds
        u64 mtime = 0;
        u64 ctime = 0;
        u32 links = 0;  // the names that are links to it
        u32 opens = 0;  // the endpoints open on it
        u32 holds = 0;  // what else points at it: directories under it, files opened in it by name
        bool released = false;  // gone from its server, its memory kept for what holds it
        Kind kind = Kind::Dir;
        i32 exclusive = -1;  // the endpoint that holds flock's exclusive lock, or -1
        i32 fd = -1;  // an object's
        SmallVec<i32> shared;  // the endpoints that hold flock's shared lock
        SmallVec<Range> ranges;
        Content content;  // a file's, a link's
        // a directory's
        Node* parent = nullptr;  // where ".." goes; none for the root
        Entry* entry = nullptr;  // its name in parent
        Server* served = nullptr;  // a server of this process mounted here
        u64 entries = 0;  // the last position given out
        u32 names = 0;  // the names in it
        u32 subdirs = 0;  // the directories among them
        u32 holes = 0;
        SmallVec<Slot> order;
    };

    struct Server {
        Node* root = nullptr;
        bool writable = false;
        u64 capacity = 0;  // the bytes a writable server's files may hold
        u64 inodes = 0;  // the inode numbers given out
        u64 nodes = 0;  // the nodes there are
        u64 used = 0;  // the bytes the files hold
        u64 dev = 0;  // the device of the server's files
        OpenTable<Node> byIno;  // the nodes there are, by inode
        Node* above = nullptr;  // where the server is mounted, of aboveServer
        Server* aboveServer = nullptr;

        Node* find(u64 ino) const;
    };

    // A node of one of this process's servers.
    struct Place {
        Server* s;
        Node* node;
    };

    struct Endpoint {
        i32 fd;
        bool reads;  // what its open lets it do
        bool writes;
        u8 nameLength;
        Server* server;
        Node* node;  // what the endpoint is open on
        Node* dir;  // the directory a file was opened in, by name
        Endpoint* next;  // among the free ones
        u8 name[nameMax];
    };

    static u32 hashIno(u64 ino) {
        return finish(ino * 0x9e3779b97f4a7c15ull);
    }

    Node* Server::find(u64 ino) const {
        return byIno.find(hashIno(ino), [ino](const Node* n) {
            return n->ino == ino;
        });
    }

    static u32 hashName(const Node* dir, const u8* name, usize size) {
        u64 h = 0x9e3779b97f4a7c15ull ^ (u64(uptr(dir)) << 20) ^ size;
        usize i = 0;
        for (; i + 8 <= size; i += 8) {
            u64 word;
            __builtin_memcpy(&word, name + i, 8);
            h = (h ^ word) * 0xff51afd7ed558ccdull;
            h ^= h >> 32;
        }
        u64 tail = 0;
        for (usize k = 0; i + k < size; ++k) {
            tail |= u64(name[i + k]) << (8 * k);
        }
        return finish((h ^ tail) * 0xc4ceb9fe1a85ec53ull);
    }

    // The names of all directories, by directory and name.
    OpenTable<Entry> names;

    static Entry* lookup(const Node* dir, Name name) {
        return names.find(hashName(dir, name.at, name.length), [dir, name](const Entry* e) {
            return e->dir == dir && e->length == name.length && (name.length == 0 || memcmp(e->name, name.at, name.length) == 0);
        });
    }

    static u64 now() {
        timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        return u64(ts.tv_sec) * 1000000000ull + u64(ts.tv_nsec);
    }

    static bool dead(const Node* n) {
        return n->released && n->links == 0 && n->opens == 0 && n->holds == 0;
    }

    // A released node nothing points at any more: its memory goes, and
    // with it its hold on its parent.
    static void destroy(Node* n) {
        while (n) {
            Node* parent = n->kind == Kind::Dir ? n->parent : nullptr;
            n->~Node();
            free(n);
            n = parent && --parent->holds == 0 && dead(parent) ? parent : nullptr;
        }
    }

    static void hold(Node* n) {
        ++n->holds;
    }

    static void drop(Node* n) {
        if (--n->holds == 0 && dead(n)) {
            destroy(n);
        }
    }

    // Puts a name for a node in a directory, after every entry there: at
    // the next position. copy: the name's bytes are kept with the entry,
    // else they are the image's.
    static void place(Node* dir, Name name, Node* n, u64 t, bool copy = true) {
        auto* e = static_cast<Entry*>(allocate(checkedAdd(sizeof(Entry), copy ? name.length : 0)));
        if (copy) {
            auto* bytes = reinterpret_cast<u8*>(e + 1);
            if (name.length) {
                memcpy(bytes, name.at, name.length);
            }
            e->name = bytes;
        } else {
            e->name = name.at;
        }
        e->dir = dir;
        e->node = n;
        e->pos = ++dir->entries;
        e->length = u32(name.length);
        e->hash = hashName(dir, name.at, name.length);
        names.insert(e->hash, e);
        dir->order.pushBack({e->pos, e});
        ++dir->names;
        ++n->links;
        if (n->kind == Kind::Dir) {
            ++dir->subdirs;
            n->entry = e;
            if (n->parent != dir) {
                hold(dir);
                if (n->parent) {
                    drop(n->parent);
                }
                n->parent = dir;
            }
        }
        dir->mtime = t;
        dir->ctime = t;
        n->ctime = t;
    }

    // Takes a name out of a directory: the node it named, one link fewer.
    static Node* unplace(Node* dir, Entry* e, u64 t) {
        Node* n = e->node;
        names.erase(e->hash, e);
        Slot* slots = dir->order.data();
        const usize count = dir->order.size();
        usize lo = 0;
        usize hi = count;
        while (lo < hi) {
            const usize mid = lo + (hi - lo) / 2;
            if (slots[mid].pos < e->pos) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        if (lo == count || slots[lo].entry != e) {
            fatal("rootfs: an entry out of its directory's order");
        }
        slots[lo].entry = nullptr;
        if (++dir->holes * 2 > count) {
            dir->order.keepIf([](const Slot& slot) {
                return slot.entry != nullptr;
            });
            dir->holes = 0;
        }
        --dir->names;
        if (n->kind == Kind::Dir) {
            --dir->subdirs;
            n->entry = nullptr;
        }
        --n->links;
        free(e);
        dir->mtime = t;
        dir->ctime = t;
        n->ctime = t;
        return n;
    }

    // A device for a server's files: the number of a pair, which no other
    // pair in the kernel has, as it never gives a number twice.
    static u64 device() {
        int ends[2];
        if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, ends) != 0) {
            fatal("socketpair");
        }
        struct stat st;
        if (fstat(ends[0], &st) != 0) {
            fatal("fstat");
        }
        close(ends[0]);
        close(ends[1]);
        return u64(st.st_ino);
    }

    // A new node, with the permissions of its kind until they are set; the
    // first gives the server its device.
    static Node* makeNode(Server& s, Node* parent, Name name, Kind kind, bool copy = true) {
        if (s.dev == 0) {
            s.dev = device();
        }
        ++s.inodes;
        ++s.nodes;
        const u64 t = now();
        auto* n = new (allocate(sizeof(Node))) Node();
        n->ino = s.inodes;
        n->kind = kind;
        n->atime = t;
        n->mtime = t;
        n->ctime = t;
        switch (kind) {
        case Kind::Dir:
            n->perm = 0755;
            break;
        case Kind::File:
        case Kind::Fifo:
            n->perm = 0644;
            break;
        case Kind::Link:
            n->perm = 0777;
            break;
        default:
            n->perm = 0666;
        }
        s.byIno.insert(hashIno(n->ino), n);
        if (parent) {
            place(parent, name, n, t, copy);
        }
        return n;
    }

    // A node with no name and no endpoint is gone: what it held is the
    // server's again. The root has no name, and stays.
    static void release(Server& s, Node* n) {
        if (n->links != 0 || n->opens != 0 || n == s.root) {
            return;
        }
        if (!n->released) {
            if (n->kind == Kind::File) {
                s.used -= n->content.size();
                n->content.own.clear();
                n->content.view = nullptr;
                n->content.viewSize = 0;
            }
            s.byIno.erase(hashIno(n->ino), n);
            --s.nodes;
            n->released = true;
        }
        if (n->holds == 0) {
            destroy(n);
        }
    }

    // Whether a writable server has no node left to make.
    static bool full(const Server& s) {
        return s.capacity > 0 && s.nodes >= s.capacity / blockSize;
    }

    // A file's size set, its bytes counted against the capacity: false
    // when they do not fit, ENOSPC.
    static bool resize(Server& s, Node* n, u64 size) {
        const u64 old = n->content.size();
        if (size > old && s.capacity > 0 && s.used + (size - old) > s.capacity) {
            return false;
        }
        if (size > ~usize(0)) {
            return false;
        }
        n->content.own.resizeZeroed(usize(size));
        if (size > old) {
            s.used += size - old;
        } else {
            s.used -= old - size;
        }
        return true;
    }

    // A file's data, counted as the server's: the image's bytes, for the
    // server that fills its tree from them.
    static void fill(Server& s, Node* n, const u8* data, usize size) {
        s.used -= n->content.size();
        n->content.own.clear();
        n->content.view = data;
        n->content.viewSize = size;
        s.used += size;
    }

    // Whether a server is mounted on the node.
    static bool mountPoint(const Node* n) {
        return n->kind == Kind::Dir && n->served;
    }

    // Whether who asks may read (4), write (2), and search or execute (1)
    // the node.
    static bool may(const Node* n, const Header& h, u32 want) {
        if (h.uid == 0) {
            return (want & 1) == 0 || n->kind == Kind::Dir || (n->perm & 0111) != 0;
        }
        const u32 bits = h.uid == n->uid ? n->perm >> 6 : h.gid == n->gid ? n->perm >> 3 : n->perm;
        return (bits & want) == want;
    }

    static bool owns(const Node* n, const Header& h) {
        return h.uid == 0 || h.uid == n->uid;
    }

    // Whether a sticky directory keeps a name from who asks: neither the
    // node it names nor the directory is theirs.
    static bool keeps(const Node* dir, const Node* n, const Header& h) {
        return (dir->perm & sSticky) != 0 && !owns(n, h) && h.uid != dir->uid;
    }

    // ---- places ------------------------------------------------------------------

    // A walk goes from place to place: into the root of a server mounted on
    // a name, and by ".." from such a root to the parent of where it is
    // mounted, never above the root of the process that asks.

    Server image;  // the root
    Server devices;  // /dev
    Server temporary;  // /tmp
    Server* const everyServer[] = {&image, &devices, &temporary};

    // Whether a place is the node of a device and inode.
    static bool at(Place p, u64 dev, u64 ino) {
        return p.s->dev == dev && p.node->ino == ino;
    }

    // The place of a device and inode, or none of this process.
    static Place found(u64 dev, u64 ino) {
        for (Server* s : everyServer) {
            if (s->dev == dev) {
                return {s, s->find(ino)};
            }
        }
        return {nullptr, nullptr};
    }

    // Where a name goes: the root of a server mounted on it, or its node.
    static Place down(Place p) {
        while (p.node->kind == Kind::Dir && p.node->served) {
            Server* s = p.node->served;
            p = {s, s->root};
        }
        return p;
    }

    // Where ".." goes from a directory: nowhere from the root of the
    // process, the node of dev and ino; from the root of a mounted server,
    // to the parent of where it is mounted; else to the parent.
    static Place up(Place p, u64 dev, u64 ino) {
        while (!at(p, dev, ino)) {
            if (p.node != p.s->root) {
                return {p.s, p.node->parent};
            }
            if (!p.s->above) {
                return p;
            }
            p = {p.s->aboveServer, p.s->above};
        }
        return p;
    }

    // Why a name is none a call takes, 0 for one it does: "." and ".." and
    // names with "/" are a walk's; a name past nameMax is too long.
    static int badName(Name name) {
        if (name.is(".") || name.is("..") || name.has('/')) {
            return EINVAL;
        }
        if (name.length > nameMax) {
            return ENAMETOOLONG;
        }
        return 0;
    }

    // What a name of a directory is to a call that looks at it: the
    // directory itself for "", else the name's node, or the root of a
    // server mounted on it; e says why there is none.
    static Place named(Server* s, Node* c, Name name, const Header& h, int& e) {
        if (name.length == 0) {
            return {s, c};
        }
        e = badName(name);
        Entry* entry = nullptr;
        if (e == 0 && c->kind != Kind::Dir) {
            e = ENOTDIR;
        } else if (e == 0 && !may(c, h, 1)) {
            e = EACCES;
        } else if (e == 0 && !(entry = lookup(c, name))) {
            e = ENOENT;
        }
        if (e != 0) {
            return {nullptr, nullptr};
        }
        return down({s, entry->node});
    }

    // Whether a directory was removed: it takes no name.
    static bool gone(const Server* s, const Node* c) {
        return c->kind == Kind::Dir && c->links == 0 && c != s->root;
    }

    // Why a name cannot be made in a directory, 0 when it can. A name that
    // is there is EEXIST before anything else, on a server that writes
    // nothing too: mkdir -p asks for each directory.
    static int creates(const Server* s, Node* c, Name name, const Header& h) {
        if (const int e = badName(name)) {
            return e;
        }
        if (c->kind != Kind::Dir) {
            return ENOTDIR;
        }
        if (!may(c, h, 1)) {
            return EACCES;
        }
        if (lookup(c, name)) {
            return EEXIST;
        }
        if (gone(s, c)) {
            return ENOENT;
        }
        if (!s->writable) {
            return EROFS;
        }
        if (!may(c, h, 3)) {
            return EACCES;
        }
        if (full(*s)) {
            return ENOSPC;
        }
        return 0;
    }

    static u32 modeOf(const Node* n) {
        switch (n->kind) {
        case Kind::Dir:
            return sIfdir | n->perm;
        case Kind::File:
            return sIfreg | n->perm;
        case Kind::Link:
            return sIflnk | n->perm;
        case Kind::Fifo:
            return sIfifo | n->perm;
        default:
            return sIfchr | n->perm;
        }
    }

    static u8 dtype(const Node* n) {
        switch (n->kind) {
        case Kind::Dir:
            return dtDir;
        case Kind::File:
            return dtReg;
        case Kind::Link:
            return dtLnk;
        case Kind::Fifo:
            return dtFifo;
        default:
            return dtChr;
        }
    }

    // A directory's: its name, its ".", and the ".." of each directory in
    // it; another's: its names.
    static u32 nlink(const Node* n) {
        return n->kind == Kind::Dir ? 2 + n->subdirs : n->links;
    }

    static Stat statOf(const Server& s, const Node* n) {
        Stat st{};
        st.ino = n->ino;
        st.mode = modeOf(n);
        st.nlink = nlink(n);
        st.uid = n->uid;
        st.gid = n->gid;
        st.atime = n->atime;
        st.mtime = n->mtime;
        st.dev = s.dev;
        st.ctime = n->ctime;
        if (n->kind == Kind::File || n->kind == Kind::Link) {
            st.size = n->content.size();
        }
        return st;
    }

    // A writable server's capacity, in blocks and in nodes, and what is
    // left of it; a read-only one's what it holds, nothing left.
    static Statfs statfsOf(const Server& s) {
        Statfs st{};
        st.bsize = u32(blockSize);
        st.namemax = u32(nameMax);
        st.dev = s.dev;
        const u64 used = (s.used + blockSize - 1) / blockSize;
        if (s.writable && s.capacity > 0) {
            st.blocks = s.capacity / blockSize;
            st.bfree = st.blocks - least(used, u64(st.blocks));
            st.files = s.capacity / blockSize;
            st.ffree = st.files - least(s.nodes, u64(st.files));
        } else {
            st.blocks = used;
            st.files = s.nodes;
        }
        if (!s.writable) {
            st.flags = stRdonly;
        }
        return st;
    }

    // ---- packets -----------------------------------------------------------------

    // The packet recv takes and the descriptors that come with it, the room
    // for them, the room for the descriptors a reply sends, and the bytes a
    // reply assembles: one of each for the process, whose loop takes a
    // packet and is done with it before the next, and sends each reply
    // whole. zeros is what /dev/zero reads, never written.
    alignas(16) u8 packet[maxPacket];
    alignas(cmsghdr) u8 packetControl[64];
    i32 packetFds[maxFds];
    usize packetFdCount;
    alignas(cmsghdr) u8 sendControl[64];
    alignas(16) u8 out[maxPacket - sizeof(Header)];
    u8 zeros[maxPacket - sizeof(Header)];

    static_assert(CMSG_SPACE(maxFds * sizeof(i32)) <= sizeof packetControl);
    static_assert(CMSG_SPACE(maxFds * sizeof(i32)) <= sizeof sendControl);

    // A reply: the header, then the parts where they are.
    static void reply(i32 fd, u32 op, u32 arg, const iovec* parts, usize count, const i32* fds = nullptr, usize fdCount = 0) {
        Header head{op, arg, 0, 0};
        iovec iov[4];
        if (count > 3 || fdCount > maxFds) {
            fatal("rootfs: a reply of too many parts");
        }
        iov[0] = {&head, sizeof head};
        for (usize i = 0; i < count; ++i) {
            iov[1 + i] = parts[i];
        }
        msghdr m{};
        m.msg_iov = iov;
        m.msg_iovlen = int(1 + count);
        if (fdCount > 0) {
            memset(sendControl, 0, sizeof sendControl);
            m.msg_control = sendControl;
            m.msg_controllen = socklen_t(CMSG_LEN(fdCount * sizeof(i32)));
            cmsghdr* c = CMSG_FIRSTHDR(&m);
            c->cmsg_level = SOL_SOCKET;
            c->cmsg_type = SCM_RIGHTS;
            c->cmsg_len = m.msg_controllen;
            memcpy(CMSG_DATA(c), fds, fdCount * sizeof(i32));
        }
        (void)sendmsg(fd, &m, 0);  // a gone reader is its own loss
    }

    static iovec part(const void* data, usize size) {
        return {const_cast<void*>(data), size};
    }

    static void ok(i32 fd) {
        reply(fd, opOk, 0, nullptr, 0);
    }

    static void ok(i32 fd, const void* data, usize size) {
        const iovec body = part(data, size);
        reply(fd, opOk, 0, &body, size ? 1 : 0);
    }

    static void err(i32 fd, int e) {
        reply(fd, opErr, u32(e), nullptr, 0);
    }

    // The reply of a call that changes something: OK, or the error.
    static void answer(i32 fd, int e) {
        if (e == 0) {
            ok(fd);
        } else {
            err(fd, e);
        }
        close(fd);
    }

    // One packet into packet, its descriptors into packetFds: the length
    // of its body, after the header, or -1 at the end of the endpoint.
    static i64 receive(i32 fd, Header& h) {
        iovec iov{packet, sizeof packet};
        msghdr m{};
        m.msg_iov = &iov;
        m.msg_iovlen = 1;
        m.msg_control = packetControl;
        m.msg_controllen = socklen_t(CMSG_SPACE(maxFds * sizeof(i32)));
        const ssize_t n = recvmsg(fd, &m, msgCmsgCloexec);
        if (n <= 0) {
            return -1;
        }
        if (usize(n) < sizeof(Header)) {
            fatal("a packet without a header");
        }
        memcpy(&h, packet, sizeof h);
        packetFdCount = 0;
        for (cmsghdr* c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c)) {
            if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS || c->cmsg_len < CMSG_LEN(0)) {
                continue;
            }
            const usize count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(i32);
            for (usize i = 0; i < count && packetFdCount < maxFds; ++i) {
                memcpy(&packetFds[packetFdCount++], CMSG_DATA(c) + i * sizeof(i32), sizeof(i32));
            }
        }
        return i64(n) - i64(sizeof(Header));
    }

    static void put32(u8* at, u32 v) {
        __builtin_memcpy(at, &v, 4);
    }

    static void put64(u8* at, u64 v) {
        __builtin_memcpy(at, &v, 8);
    }

    // ---- the loop's endpoints -------------------------------------------------------
    //
    // One epoll of the process watches every endpoint of its servers: an
    // endpoint joins it when it is listened on, leaves it before it closes.
    // An event carries its endpoint.

    int watcher = -1;
    Endpoint* freeEndpoints;
    usize endpointCount;  // the endpoints of all servers

    // The signal mask of the wait, once reapOnChild blocked SIGCHLD
    // everywhere else: a child's end that comes while the loop works waits
    // for the wait, which it interrupts at once, instead of coming before
    // it, unseen. The handler notes it: the loop reaps after a wait a
    // child's end interrupted.
    sigset_t waitMask;
    bool waitMasked;
    volatile sig_atomic_t childEnded = 1;

    // An endpoint the server answers on, open on node, or on the root, to
    // read, to write or both; a file opened by a name of dir says it. Its
    // descriptor came with MSG_CMSG_CLOEXEC, or from a pair of ours, made
    // with SOCK_CLOEXEC.
    static void listen(Server& s, i32 fd, Node* node, bool reads, bool writes, Node* dir, Name name) {
        Node* n = node ? node : s.root;
        Endpoint* ep = freeEndpoints;
        if (ep) {
            freeEndpoints = ep->next;
        } else {
            ep = static_cast<Endpoint*>(allocate(sizeof(Endpoint)));
        }
        if (name.length > nameMax) {
            fatal("rootfs: a name too long for an endpoint");
        }
        ++n->opens;
        ep->fd = fd;
        ep->reads = reads;
        ep->writes = writes;
        ep->server = &s;
        ep->node = n;
        ep->dir = dir;
        ep->next = nullptr;
        ep->nameLength = u8(name.length);
        if (name.length) {
            memcpy(ep->name, name.at, name.length);
        }
        if (dir) {
            hold(dir);
        }
        ++endpointCount;
        if (watcher >= 0) {
            epoll_event ev{};
            ev.events = EPOLLIN;
            ev.data.u64 = u64(uptr(ep));
            if (epoll_ctl(watcher, EPOLL_CTL_ADD, fd, &ev) != 0) {
                fatal("fs: epoll_ctl");
            }
        }
    }

    static void removeShared(Node* n, i32 fd) {
        n->shared.keepIf([fd](i32 holder) {
            return holder != fd;
        });
    }

    // An endpoint that closes: its locks go with it, and its node when no
    // name is left of it.
    static void forget(Endpoint* ep) {
        Node* n = ep->node;
        const i32 fd = ep->fd;
        removeShared(n, fd);
        if (n->exclusive == fd) {
            n->exclusive = -1;
        }
        n->ranges.keepIf([fd](const Range& l) {
            return l.owner != fd;
        });
        --n->opens;
        release(*ep->server, n);
        if (ep->dir) {
            drop(ep->dir);
        }
        ep->next = freeEndpoints;
        freeEndpoints = ep;
        --endpointCount;
    }

    // ---- calls on paths and names ------------------------------------------------

    // The names of a walk: the path's, and before them the targets of the
    // links it went through, the last on top.
    struct Walk {
        Name stack[maxLinks + 1];
        u32 depth = 0;

        void push(Name n) {
            if (depth == maxLinks + 1) {
                fatal("rootfs: a walk of too many links");
            }
            stack[depth++] = n;
        }

        // The next name, or false at the end.
        bool take(Name& name) {
            while (depth > 0) {
                Name& top = stack[depth - 1];
                while (top.length && *top.at == '/') {
                    ++top.at;
                    --top.length;
                }
                if (top.length) {
                    const u8* start = top.at;
                    while (top.length && *top.at != '/') {
                        ++top.at;
                        --top.length;
                    }
                    name = {start, usize(top.at - start)};
                    return true;
                }
                --depth;
            }
            return false;
        }

        // Whether a name is left.
        bool more() const {
            for (u32 i = depth; i-- > 0;) {
                for (usize k = 0; k < stack[i].length; ++k) {
                    if (stack[i].at[k] != '/') {
                        return true;
                    }
                }
            }
            return false;
        }
    };

    // A path walked from the endpoint's directory to the directory of its
    // last name, as Linux walks one: every name but the last must be a
    // directory who asks may search, or a symbolic link, whose target takes
    // its place, from the root of the process when it is absolute, no more
    // than maxLinks of them in all; ".", "..", and the root of a mounted
    // server for its mount point, as places have it. The reply is the
    // step, the links gone through, the kind of the last name, whether a
    // slash ends the path and whether the reply's endpoint is now the
    // directory's, its own; then the last name. A walk to a root of no
    // server of this process is the step stepRoot, with the path left.
    static void resolve(Endpoint* ep, const Header& h, Body body, i32 fd) {
        const u64 dev = body.quad();
        const u64 ino = body.quad();
        u32 links = body.word();
        const Name path = body.rest();
        const bool slash = path.length > 0 && path.at[path.length - 1] == '/';
        Walk walk;
        walk.push(path);
        Place here{ep->server, ep->node};
        int e = 0;
        Name name;
        bool have = walk.take(name);
        while (e == 0 && have && walk.more()) {
            Entry* entry = nullptr;
            if (here.node->kind != Kind::Dir) {
                e = ENOTDIR;
            } else if (!may(here.node, h, 1)) {
                e = EACCES;
            } else if (name.is(".")) {
            } else if (name.is("..")) {
                here = up(here, dev, ino);
            } else if (name.length > nameMax) {
                e = ENAMETOOLONG;
            } else if (!(entry = lookup(here.node, name))) {
                e = ENOENT;
            } else {
                const Place next = down({here.s, entry->node});
                if (next.node->kind != Kind::Link) {
                    here = next;
                } else if (links >= maxLinks) {
                    e = ELOOP;
                } else if (next.node->content.size() == 0) {
                    e = ENOENT;
                } else {
                    ++links;
                    const Name target{next.node->content.data(), next.node->content.size()};
                    walk.push(target);
                    if (target.at[0] == '/') {
                        here = found(dev, ino);
                        if (!here.node) {
                            put32(out, stepRoot);
                            put32(out + 4, links);
                            put32(out + 8, lastNone);
                            put32(out + 12, 0);
                            usize size = 16;
                            Name left;
                            bool first = true;
                            while (e == 0 && walk.take(left)) {
                                if (size + 1 + left.length + 1 > sizeof out) {
                                    e = ENAMETOOLONG;
                                    break;
                                }
                                if (!first) {
                                    out[size++] = '/';
                                }
                                first = false;
                                memcpy(out + size, left.at, left.length);
                                size += left.length;
                            }
                            if (e == 0 && slash) {
                                out[size++] = '/';
                            }
                            if (e == 0) {
                                ok(fd, out, size);
                            } else {
                                err(fd, e);
                            }
                            close(fd);
                            return;
                        }
                    }
                }
            }
            if (e == 0) {
                have = walk.take(name);
            }
        }
        if (e == 0 && here.node->kind != Kind::Dir) {
            e = ENOTDIR;
        }
        if (e == 0 && have && !may(here.node, h, 1)) {
            e = EACCES;
        }
        u32 last = lastNone;
        Name final;
        if (e == 0 && have) {
            if (name.is(".")) {
                last = lastDot;
            } else if (name.is("..")) {
                last = lastDotdot;
                here = up(here, dev, ino);
            } else if (name.length > nameMax) {
                e = ENAMETOOLONG;
            } else {
                last = lastName;
                final = name;
            }
        }
        if (e != 0) {
            err(fd, e);
            close(fd);
            return;
        }
        const bool kept = here.s != ep->server || here.node != ep->node;
        u32 head[4] = {stepDone, links, last, (slash && last == lastName ? stepSlash : 0) | (kept ? stepKept : 0)};
        const iovec parts[2] = {part(head, sizeof head), part(final.at, final.length)};
        if (kept) {
            listen(*here.s, fd, here.node, false, false, nullptr, {});
            reply(fd, opOk, 0, parts, final.length ? 2 : 1);
        } else {
            reply(fd, opOk, 0, parts, final.length ? 2 : 1);
            close(fd);
        }
    }

    // The path of an endpoint's directory from the root of the process,
    // the node of a device and inode, up through the places ".." goes to;
    // of a file, its directory's and the name it was opened by. A removed
    // directory has none, nor one the root is not above: ENOENT. The path
    // is made from its end, at the end of out.
    static void pathOf(Endpoint* ep, Body body, i32 fd) {
        const u64 dev = body.quad();
        const u64 ino = body.quad();
        Place here{ep->server, ep->node};
        usize start = sizeof out;
        int e = 0;
        auto prepend = [&](const u8* name, usize size) {
            if (start < size + 1) {
                e = ENAMETOOLONG;
                return;
            }
            start -= size;
            memcpy(out + start, name, size);
            out[--start] = '/';
        };
        if (here.node->kind != Kind::Dir) {
            if (!ep->dir) {
                e = ENOENT;
            } else {
                prepend(ep->name, ep->nameLength);
                here = {ep->server, ep->dir};
            }
        }
        while (e == 0 && !at(here, dev, ino)) {
            if (here.node == here.s->root) {
                if (!here.s->above) {
                    e = ENOENT;
                } else {
                    here = {here.s->aboveServer, here.s->above};
                }
                continue;
            }
            if (gone(here.s, here.node)) {
                e = ENOENT;
                continue;
            }
            const Entry* entry = here.node->entry;
            if (!entry || entry->length == 0) {
                e = ENOENT;
                continue;
            }
            prepend(entry->name, entry->length);
            here = {here.s, here.node->parent};
        }
        if (e != 0) {
            err(fd, e);
        } else if (start == sizeof out) {
            ok(fd, "/", 1);
        } else {
            ok(fd, out + start, sizeof out - start);
        }
        close(fd);
    }

    // A name of the directory opened, or the directory itself for "": read
    // or written as its access mode says and who asks may, made when
    // O_CREAT asks and it is not there, emptied when O_TRUNC asks; with
    // O_PATH only held, for a working directory or a root. A symbolic link
    // is ELOOP, for the kernel to follow, or held itself with O_PATH and
    // O_NOFOLLOW.
    static void open(Endpoint* ep, const Header& h, Body body, i32 fd) {
        Server* s = ep->server;
        Node* c = ep->node;
        const u32 mode = body.word();
        const Name name = body.string();
        const bool opath = (h.arg & oPath) != 0;
        const u32 access = h.arg & 3;
        const bool reads = !opath && access != 1;
        const bool writes = !opath && access != 0;
        const bool truncates = !opath && (h.arg & oTrunc) != 0;
        const bool creating = (h.arg & oCreat) != 0 && name.length > 0;
        const bool there = name.length > 0 && c->kind == Kind::Dir && lookup(c, name);
        int e = 0;
        Place target{nullptr, nullptr};
        bool made = false;
        if (creating && there && (h.arg & oExcl) != 0) {
            e = EEXIST;
        } else if (creating && !there) {
            e = creates(s, c, name, h);
            if (e == 0) {
                Node* n = makeNode(*s, c, name, Kind::File);
                n->perm = mode & 07777;
                n->uid = h.uid;
                n->gid = h.gid;
                target = {s, n};
                made = true;
            }
        } else {
            target = named(s, c, name, h, e);
        }
        if (e == 0 && target.node->kind == Kind::Link && !(opath && (h.arg & oNofollow) != 0)) {
            e = ELOOP;
        } else if (e == 0 && (h.arg & oDirectory) != 0 && target.node->kind != Kind::Dir) {
            e = ENOTDIR;
        } else if (e == 0 && !made && !opath) {
            const u32 want = (reads ? 4 : 0) | (writes || truncates ? 2 : 0);
            if (!may(target.node, h, want)) {
                e = EACCES;
            } else if (target.node->kind == Kind::Dir && (writes || (h.arg & oCreat) != 0)) {
                e = EISDIR;
            } else if ((writes || truncates) && target.node->kind == Kind::File && !target.s->writable) {
                e = EROFS;
            }
        }
        if (e != 0) {
            answer(fd, e);
            return;
        }
        if (target.node->kind == Kind::Object && !opath) {
            reply(fd, opOk, 0, nullptr, 0, &target.node->fd, 1);  // the object itself becomes the caller's
            close(fd);
            return;
        }
        if (truncates && target.node->kind == Kind::File && target.node->content.size() > 0) {
            resize(*target.s, target.node, 0);
            const u64 t = now();
            target.node->mtime = t;
            target.node->ctime = t;
        }
        const Stat st = statOf(*target.s, target.node);
        listen(*target.s, fd, target.node, reads, writes, name.length ? c : nullptr, name);
        ok(fd, &st, sizeof st);
    }

    static void stat(Endpoint* ep, const Header& h, Body body, i32 fd) {
        int e = 0;
        const Place t = named(ep->server, ep->node, body.string(), h, e);
        if (e != 0) {
            answer(fd, e);
            return;
        }
        const Stat st = statOf(*t.s, t.node);
        ok(fd, &st, sizeof st);
        close(fd);
    }

    // The file system of a name: the server's, or the one mounted there.
    static void statfs(Endpoint* ep, const Header& h, Body body, i32 fd) {
        int e = 0;
        const Place t = named(ep->server, ep->node, body.string(), h, e);
        if (e != 0) {
            answer(fd, e);
            return;
        }
        const Statfs st = statfsOf(*t.s);
        ok(fd, &st, sizeof st);
        close(fd);
    }

    static void readlink(Endpoint* ep, const Header& h, Body body, i32 fd) {
        int e = 0;
        const Place t = named(ep->server, ep->node, body.string(), h, e);
        if (e == 0 && t.node->kind != Kind::Link) {
            e = EINVAL;
        }
        if (e != 0) {
            answer(fd, e);
            return;
        }
        ok(fd, t.node->content.data(), t.node->content.size());
        close(fd);
    }

    // mkdir, mknod and symlink: a new name in the directory, who asks may
    // write, theirs, with the mode asked for. mknod makes FIFOs and regular
    // files; a device is EPERM.
    static void create(Endpoint* ep, const Header& h, Body body, i32 fd) {
        Server* s = ep->server;
        Node* c = ep->node;
        const Name target = h.op == opSymlink ? body.string() : Name{};
        const Name name = body.string();
        Kind kind = Kind::Dir;
        int e = creates(s, c, name, h);
        if (e == 0 && h.op == opSymlink) {
            kind = Kind::Link;
        } else if (e == 0 && h.op == opMknod) {
            switch (h.arg & sIfmt) {
            case sIfifo:
                kind = Kind::Fifo;
                break;
            case 0:
            case sIfreg:
                kind = Kind::File;
                break;
            default:
                e = EPERM;
            }
        }
        if (e == 0) {
            Node* n = makeNode(*s, c, name, kind);
            n->uid = h.uid;
            n->gid = h.gid;
            if (kind == Kind::Link) {
                n->content.own.append(target.at, target.length);
            } else {
                n->perm = h.arg & 07777;
            }
        }
        answer(fd, e);
    }

    static void unlink(Endpoint* ep, const Header& h, Body body, i32 fd) {
        Server* s = ep->server;
        Node* c = ep->node;
        const Name name = body.string();
        const bool removeDir = (h.arg & atRemovedir) != 0;
        int e = badName(name);
        Entry* entry = nullptr;
        if (e == 0 && c->kind != Kind::Dir) {
            e = ENOTDIR;
        } else if (e == 0 && !may(c, h, 1)) {
            e = EACCES;
        } else if (e == 0 && !s->writable) {
            e = EROFS;
        } else if (e == 0 && !(entry = lookup(c, name))) {
            e = ENOENT;
        } else if (e == 0) {
            Node* n = entry->node;
            if (removeDir && n->kind != Kind::Dir) {
                e = ENOTDIR;
            } else if (!removeDir && n->kind == Kind::Dir) {
                e = EISDIR;
            } else if (!may(c, h, 3)) {
                e = EACCES;
            } else if (keeps(c, n, h)) {
                e = EPERM;
            } else if (removeDir && mountPoint(n)) {
                e = EBUSY;
            } else if (removeDir && n->names > 0) {
                e = ENOTEMPTY;
            } else {
                release(*s, unplace(c, entry, now()));
            }
        }
        answer(fd, e);
    }

    // Whether dir is n or under it.
    static bool within(const Node* n, const Node* dir) {
        for (const Node* up = dir; up; up = up->parent) {
            if (up == n) {
                return true;
            }
        }
        return false;
    }

    // The directory of a device and inode another name of a call is in:
    // EXDEV on another server, ENOENT when it is no directory there now.
    static Node* directory(const Server* s, u64 dev, u64 ino, int& e) {
        if (e != 0) {
            return nullptr;
        }
        if (dev != s->dev) {
            e = EXDEV;
            return nullptr;
        }
        Node* n = s->find(ino);
        if (!n || n->kind != Kind::Dir || gone(s, n)) {
            e = ENOENT;
            return nullptr;
        }
        return n;
    }

    // A name of the directory moved to a name of another of the server's,
    // of a device and inode: on another server EXDEV. What the new name
    // names goes, when it is of the old one's kind, and empty if a
    // directory; a directory does not go under itself, nor a mount point
    // anywhere, and one that goes to another directory must be one who
    // asks may write, for its "..".
    static void rename(Endpoint* ep, const Header& h, Body body, i32 fd) {
        Server* s = ep->server;
        Node* src = ep->node;
        const u64 dev = body.quad();
        const u64 ino = body.quad();
        const Name oldName = body.string();
        const Name newName = body.string();
        int e = badName(oldName);
        if (e == 0) {
            e = badName(newName);
        }
        Node* dst = directory(s, dev, ino, e);
        Entry* entry = nullptr;
        if (e == 0 && src->kind != Kind::Dir) {
            e = ENOTDIR;
        } else if (e == 0 && (!may(src, h, 1) || !may(dst, h, 1))) {
            e = EACCES;
        } else if (e == 0 && !s->writable) {
            e = EROFS;
        } else if (e == 0 && !(entry = lookup(src, oldName))) {
            e = ENOENT;
        } else if (e == 0) {
            Node* n = entry->node;
            Entry* replaced = lookup(dst, newName);
            Node* old = replaced ? replaced->node : nullptr;
            if (old == n) {
            } else if (n->kind == Kind::Dir && within(n, dst)) {
                e = EINVAL;
            } else if (!may(src, h, 3) || !may(dst, h, 3)) {
                e = EACCES;
            } else if (keeps(src, n, h) || (old && keeps(dst, old, h))) {
                e = EPERM;
            } else if (n->kind == Kind::Dir && src != dst && !may(n, h, 2)) {
                e = EACCES;
            } else if (mountPoint(n) || (old && mountPoint(old))) {
                e = EBUSY;
            } else if (old && n->kind == Kind::Dir && old->kind != Kind::Dir) {
                e = ENOTDIR;
            } else if (old && n->kind != Kind::Dir && old->kind == Kind::Dir) {
                e = EISDIR;
            } else if (old && old->kind == Kind::Dir && old->names > 0) {
                e = ENOTEMPTY;
            } else {
                const u64 t = now();
                unplace(src, entry, t);
                if (replaced) {
                    release(*s, unplace(dst, replaced, t));
                }
                place(dst, newName, n, t);
            }
        }
        answer(fd, e);
    }

    // A new name, in a directory of the server's of a device and inode, for
    // the node a name of this one is, which is no directory.
    static void link(Endpoint* ep, const Header& h, Body body, i32 fd) {
        Server* s = ep->server;
        Node* src = ep->node;
        const u64 dev = body.quad();
        const u64 ino = body.quad();
        const Name oldName = body.string();
        const Name newName = body.string();
        int e = badName(oldName);
        Entry* entry = nullptr;
        if (e == 0 && src->kind != Kind::Dir) {
            e = ENOTDIR;
        } else if (e == 0 && !may(src, h, 1)) {
            e = EACCES;
        } else if (e == 0 && !(entry = lookup(src, oldName))) {
            e = ENOENT;
        }
        Node* dst = directory(s, dev, ino, e);
        if (e == 0) {
            e = creates(s, dst, newName, h);
        }
        if (e == 0 && entry->node->kind == Kind::Dir) {
            e = EPERM;
        }
        if (e == 0) {
            place(dst, newName, entry->node, now());
        }
        answer(fd, e);
    }

    // The attributes of a node: set when the server is writable, by its owner
    // or root, a group given away by the owner only to its own, the times set
    // to now by who may write it too, a size by who may write it.

    // chmod, chown, utimes or truncate of a node, by name or by endpoint,
    // their arguments in args: 0, or the error.
    static int change(Server& s, Node* node, u32 op, const Header& h, Body args) {
        const u64 t = now();
        switch (op) {
        case opChmod:
        case opFchmod:
            if (!owns(node, h)) {
                return EPERM;
            }
            node->perm = h.arg & 07777;
            break;
        case opChown:
        case opFchown: {
            const u32 uid = args.word();
            const u32 gid = args.word();
            const u32 toUid = uid == noId ? node->uid : uid;
            const u32 toGid = gid == noId ? node->gid : gid;
            if (h.uid != 0 && (h.uid != node->uid || toUid != node->uid || (toGid != node->gid && toGid != h.gid))) {
                return EPERM;
            }
            if ((toUid != node->uid || toGid != node->gid) && node->kind != Kind::Dir) {
                node->perm &= ~u32(06000);
            }
            node->uid = toUid;
            node->gid = toGid;
            break;
        }
        case opUtimes:
        case opFutimes: {
            const u64 atime = args.quad();
            const u64 mtime = args.quad();
            const bool touch = (h.arg & whenAtimeNow) != 0 && (h.arg & whenMtimeNow) != 0;
            if (touch && !owns(node, h) && !may(node, h, 2)) {
                return EACCES;
            }
            if (!touch && !owns(node, h)) {
                return EPERM;
            }
            if ((h.arg & whenAtimeKept) == 0) {
                node->atime = (h.arg & whenAtimeNow) != 0 ? t : atime;
            }
            if ((h.arg & whenMtimeKept) == 0) {
                node->mtime = (h.arg & whenMtimeNow) != 0 ? t : mtime;
            }
            break;
        }
        case opTruncate:
        case opFtruncate: {
            const u64 size = args.quad();
            if (node->kind == Kind::Dir) {
                return op == opTruncate ? EISDIR : EINVAL;
            }
            if (node->kind != Kind::File || size > u64(0x7fffffff) * 1024) {
                return EINVAL;
            }
            if (op == opTruncate && !may(node, h, 2)) {
                return EACCES;
            }
            if (!resize(s, node, size)) {
                return ENOSPC;
            }
            node->mtime = t;
            break;
        }
        }
        node->ctime = t;
        return 0;
    }

    // chmod, chown, utimes and truncate of a name, or of the directory.
    static void attr(Endpoint* ep, const Header& h, Body body, i32 fd) {
        const usize ahead = h.op == opChown || h.op == opTruncate ? 8 : h.op == opUtimes ? 16 : 0;
        if (body.size() < ahead) {
            fatal("a short packet");
        }
        const Body args{body.at, body.at + ahead};
        Body rest{body.at + ahead, body.end};
        int e = 0;
        const Place t = named(ep->server, ep->node, rest.string(), h, e);
        if (e == 0 && !t.s->writable) {
            e = EROFS;
        }
        if (e == 0) {
            e = change(*t.s, t.node, h.op, h, args);
        }
        answer(fd, e);
    }

    // fchmod, fchown, futimes, ftruncate and fsync of an open node; only an
    // endpoint open to write truncates.
    static void fattr(Endpoint* ep, const Header& h, Body body, i32 fd) {
        if (h.op == opFsync) {
            ok(fd);
        } else if (!ep->server->writable) {
            err(fd, EROFS);
        } else if (h.op == opFtruncate && !ep->writes) {
            err(fd, EINVAL);
        } else if (const int e = change(*ep->server, ep->node, h.op, h, body)) {
            err(fd, e);
        } else {
            ok(fd);
        }
    }

    // ---- calls on a node ---------------------------------------------------------

    // A request's offset, or EINVAL answered for one no file has.
    static bool offset(i32 fd, Body& body, u64& off) {
        off = body.quad();
        if (off > offsetMax) {
            err(fd, EINVAL);
            return false;
        }
        return true;
    }

    static void read(Endpoint* ep, const Header& h, Body body, i32 fd) {
        u64 off;
        if (!offset(fd, body, off)) {
            return;
        }
        const Node* node = ep->node;
        const usize n = h.arg;
        if (node->kind != Kind::Dir && !ep->reads) {
            err(fd, EBADF);
            return;
        }
        switch (node->kind) {
        case Kind::File: {
            const usize size = node->content.size();
            const usize count = off >= size ? 0 : least(n, size - usize(off));
            ok(fd, node->content.data() + (count ? usize(off) : 0), count);
            break;
        }
        case Kind::Null:
            ok(fd);
            break;
        case Kind::Zero:
            ok(fd, zeros, least(n, sizeof zeros));
            break;
        case Kind::Random: {
            const usize count = least(n, sizeof out);
            for (usize at = 0; at < count; at += 256) {
                if (getentropy(out + at, least(usize(256), count - at)) != 0) {
                    err(fd, EIO);
                    return;
                }
            }
            ok(fd, out, count);
            break;
        }
        default:
            err(fd, EISDIR);
        }
    }

    // The data at the offset, or at the end with append, as much as the
    // capacity takes: the count and where it went.
    static void write(Endpoint* ep, const Header& h, Body body, i32 fd) {
        u64 off;
        if (!offset(fd, body, off)) {
            return;
        }
        Server& s = *ep->server;
        Node* node = ep->node;
        const u8* data = body.at;
        const usize length = body.size();
        if (node->kind != Kind::Dir && !ep->writes) {
            err(fd, EBADF);
            return;
        }
        u8 result[12];
        switch (node->kind) {
        case Kind::Null:
        case Kind::Zero:
        case Kind::Random:
            put32(result, u32(length));
            put64(result + 4, off);
            ok(fd, result, sizeof result);
            break;
        case Kind::File: {
            if (!s.writable) {
                err(fd, EROFS);
                return;
            }
            const u64 size = node->content.size();
            if ((h.arg & 1) != 0) {
                off = size;
            }
            u64 count = length;
            const u64 grows = off + count > size ? off + count - size : 0;
            if (s.capacity > 0 && grows > s.capacity - s.used) {
                const u64 over = grows - (s.capacity - s.used);
                count = count > over ? count - over : 0;
            }
            if (count == 0 && length > 0) {
                err(fd, ENOSPC);
                return;
            }
            if (size < off + count && !resize(s, node, off + count)) {
                err(fd, ENOSPC);
                return;
            }
            if (count) {
                memcpy(static_cast<u8*>(node->content.own.data()) + usize(off), data, usize(count));
            }
            const u64 t = now();
            node->mtime = t;
            node->ctime = t;
            put32(result, u32(count));
            put64(result + 4, off);
            ok(fd, result, sizeof result);
            break;
        }
        default:
            err(fd, EISDIR);
        }
    }

    // The entries after a position, in order, as many as one reply holds.
    static void readdir(const Node* node, Body body, i32 fd) {
        if (node->kind != Kind::Dir) {
            err(fd, ENOTDIR);
            return;
        }
        const u64 after = body.quad();
        const Slot* slots = node->order.data();
        const usize count = node->order.size();
        usize lo = 0;
        usize hi = count;
        while (lo < hi) {
            const usize mid = lo + (hi - lo) / 2;
            if (slots[mid].pos <= after) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        usize size = 0;
        for (usize i = lo; i < count; ++i) {
            const Entry* e = slots[i].entry;
            if (!e) {
                continue;
            }
            if (size + 17 + e->length + 1 > sizeof out) {
                break;
            }
            put64(out + size, e->node->ino);
            put64(out + size + 8, e->pos);
            out[size + 16] = dtype(e->node);
            memcpy(out + size + 17, e->name, e->length);
            out[size + 17 + e->length] = 0;
            size += 17 + e->length + 1;
        }
        ok(fd, out, size);
    }

    // flock's lock of the endpoint's node: shared, which no other
    // endpoint's exclusive one is in the way of, exclusive, which no other
    // endpoint's lock is, or none.
    static void flock(Endpoint* ep, const Header& h, i32 fd) {
        Node* n = ep->node;
        const i32 me = ep->fd;
        const bool other = n->exclusive >= 0 && n->exclusive != me;
        usize others = 0;
        for (usize i = 0; i < n->shared.size(); ++i) {
            others += n->shared[i] != me;
        }
        switch (h.arg & ~lockNb) {
        case lockUn:
            removeShared(n, me);
            if (n->exclusive == me) {
                n->exclusive = -1;
            }
            break;
        case lockSh:
            if (other) {
                err(fd, EAGAIN);
                return;
            }
            n->exclusive = -1;
            removeShared(n, me);
            n->shared.pushBack(me);
            break;
        case lockEx:
            if (other || others > 0) {
                err(fd, EAGAIN);
                return;
            }
            removeShared(n, me);
            n->exclusive = me;
            break;
        default:
            err(fd, EINVAL);
            return;
        }
        ok(fd);
    }

    // The ranges a LOCK keeps, made here and swapped in: one of each for
    // the process.
    [[clang::no_destroy]] SmallVec<Range> keptRanges;

    // fcntl's locks of the endpoint's node, on a range, to the end and past
    // with a length of 0. With get, the first lock of another endpoint in
    // the way of the one described, else F_UNLCK. Else that lock is set in
    // place of what the endpoint held of the range, unless another
    // endpoint's lock is in the way: EAGAIN; F_UNLCK lets the range go.
    static void lock(Endpoint* ep, const Header& h, Body body, i32 fd) {
        const u32 kind = body.word();
        const u64 start = body.quad();
        const u64 length = body.quad();
        const u64 stop = length == 0 || start > past - length ? past : start + length;
        if (kind > fUnlck) {
            err(fd, EINVAL);
            return;
        }
        Node* n = ep->node;
        const i32 me = ep->fd;
        const Range* way = nullptr;
        for (usize i = 0; i < n->ranges.size(); ++i) {
            const Range& l = n->ranges[i];
            if (l.owner != me && l.start < stop && start < l.stop && (l.write || kind == fWrlck)) {
                way = &l;
                break;
            }
        }
        if (h.arg == 1) {
            u8 got[20];
            if (way) {
                put32(got, way->write ? fWrlck : fRdlck);
                put64(got + 4, way->start);
                put64(got + 12, way->stop == past ? 0 : way->stop - way->start);
            } else {
                put32(got, fUnlck);
                put64(got + 4, start);
                put64(got + 12, length);
            }
            ok(fd, got, sizeof got);
            return;
        }
        if (kind != fUnlck && way) {
            err(fd, EAGAIN);
            return;
        }
        keptRanges.clear();
        for (usize i = 0; i < n->ranges.size(); ++i) {
            const Range l = n->ranges[i];
            if (l.owner != me || l.stop <= start || l.start >= stop) {
                keptRanges.pushBack(l);
                continue;
            }
            if (l.start < start) {
                keptRanges.pushBack({l.owner, l.write, l.start, start});
            }
            if (l.stop > stop) {
                keptRanges.pushBack({l.owner, l.write, stop, l.stop});
            }
        }
        if (kind != fUnlck) {
            keptRanges.pushBack({me, kind == fWrlck, start, stop});
        }
        n->ranges.buffer.swap(keptRanges.buffer);
        ok(fd);
    }

    // ---- the loop ---------------------------------------------------------------

    // A request on an endpoint.
    static void dispatch(Endpoint* ep, const Header& h, Body body) {
        if (packetFdCount == 0) {
            return;  // a request without its reply endpoint: nobody to answer
        }
        const i32 fd = packetFds[packetFdCount - 1];
        switch (h.op) {
        case opResolve:
            resolve(ep, h, body, fd);
            break;
        case opPath:
            pathOf(ep, body, fd);
            break;
        case opOpen:
            open(ep, h, body, fd);
            break;
        case opStat:
            stat(ep, h, body, fd);
            break;
        case opStatfs:
            statfs(ep, h, body, fd);
            break;
        case opReadlink:
            readlink(ep, h, body, fd);
            break;
        case opMkdir:
        case opMknod:
        case opSymlink:
            create(ep, h, body, fd);
            break;
        case opUnlink:
            unlink(ep, h, body, fd);
            break;
        case opRename:
            rename(ep, h, body, fd);
            break;
        case opLink:
            link(ep, h, body, fd);
            break;
        case opChmod:
        case opChown:
        case opUtimes:
        case opTruncate:
            attr(ep, h, body, fd);
            break;
        case opFchmod:
        case opFchown:
        case opFutimes:
        case opFtruncate:
        case opFsync:
            fattr(ep, h, body, fd);
            close(fd);
            break;
        case opRead:
            read(ep, h, body, fd);
            close(fd);
            break;
        case opWrite:
            write(ep, h, body, fd);
            close(fd);
            break;
        case opReaddir:
            readdir(ep->node, body, fd);
            close(fd);
            break;
        case opFstat: {
            const Stat st = statOf(*ep->server, ep->node);
            ok(fd, &st, sizeof st);
            close(fd);
            break;
        }
        case opFstatfs: {
            const Statfs st = statfsOf(*ep->server);
            ok(fd, &st, sizeof st);
            close(fd);
            break;
        }
        case opFlock:
            flock(ep, h, fd);
            close(fd);
            break;
        case opLock:
            lock(ep, h, body, fd);
            close(fd);
            break;
        case opOk:
        case opErr:
            for (usize i = 0; i < packetFdCount; ++i) {
                close(packetFds[i]);
            }
            break;
        default:
            err(fd, ENOSYS);
            close(fd);
        }
    }

    static void handle(Endpoint* ep) {
        Header h;
        const i64 n = receive(ep->fd, h);
        if (n < 0) {
            const i32 fd = ep->fd;
            epoll_ctl(watcher, EPOLL_CTL_DEL, fd, nullptr);
            forget(ep);
            close(fd);  // the other end closed: the file is closed
            return;
        }
        dispatch(ep, h, Body{packet + sizeof(Header), packet + sizeof(Header) + n});
    }

    pid_t shellPid;
    int shellStatus = -1;

    // After a wait a child's end interrupted: what ended is reaped; the
    // shell's end is the system's.
    static bool reap() {
        if (!childEnded) {
            return true;
        }
        childEnded = 0;
        for (;;) {
            int status;
            const pid_t pid = waitpid(-1, &status, WNOHANG);
            if (pid <= 0) {
                return shellStatus < 0;
            }
            if (pid == shellPid) {
                shellStatus = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
            }
        }
    }

    // Serves the servers until the last endpoint of them all closes: then
    // nobody can reach them; or until the shell ends.
    static void serve() {
        epoll_event events[64];
        for (;;) {
            if (endpointCount == 0) {
                close(watcher);
                watcher = -1;
                return;
            }
            const int n = epoll_pwait(watcher, events, 64, -1, waitMasked ? &waitMask : nullptr);
            if (!reap()) {
                return;
            }
            for (int i = 0; i < n; ++i) {
                handle(reinterpret_cast<Endpoint*>(uptr(events[i].data.u64)));
            }
        }
    }

    // ---- the system ----------------------------------------------------------------

    // Mounts a server of this process on a directory of ours, a path of
    // names from our root: a walk goes into its root there, and by ".."
    // from its root back here.
    static void mountOn(Server& s, const char* path, Server& target) {
        Node* n = s.root;
        Walk walk;
        walk.push(literal(path));
        Name name;
        while (walk.take(name)) {
            const Entry* entry = n->kind == Kind::Dir ? lookup(n, name) : nullptr;
            if (!entry) {
                fatal("mount on ", path);
            }
            n = entry->node;
        }
        if (n->kind != Kind::Dir) {
            fatal("mount on ", path);
        }
        n->served = &target;
        target.above = n;
        target.aboveServer = &s;
    }

    static void onChild(int) {
        childEnded = 1;
    }

    // Has a child's end interrupt the server's wait, and nothing else: the
    // loop then reaps. SIGCHLD is blocked but in the wait, and the mask
    // before is the wait's and, through spawn, a child's.
    static void reapOnChild() {
        sigset_t chld;
        sigemptyset(&chld);
        sigaddset(&chld, SIGCHLD);
        signal(SIGCHLD, onChild);
        if (sigprocmask(SIG_BLOCK, &chld, &waitMask) != 0) {
            fatal("rootfs: sigprocmask");
        }
        waitMasked = true;
    }

    // A field of a header block: its bytes without the NULs around them.
    static Name field(const u8* at, usize size) {
        usize start = 0;
        usize end = size;
        while (start < end && at[start] == 0) {
            ++start;
        }
        while (end > start && at[end - 1] == 0) {
            --end;
        }
        return {at + start, end - start};
    }

    static u64 octal(const u8* at, usize size) {
        u64 v = 0;
        for (usize i = 0; i < size; ++i) {
            if (at[i] >= '0' && at[i] <= '7') {
                v = v * 8 + (at[i] - '0');
            }
        }
        return v;
    }

    // The directory of a path, made on the way.
    static Node* dirOf(Server& s, Name path) {
        Node* n = s.root;
        Walk walk;
        walk.push(path);
        Name part;
        while (walk.take(part)) {
            const Entry* entry = lookup(n, part);
            n = entry ? entry->node : makeNode(s, n, part, Kind::Dir, false);
        }
        return n;
    }

    // A name of the image made again: what it named goes.
    static void unname(Server& s, Node* dir, Name name) {
        if (Entry* entry = lookup(dir, name)) {
            release(s, unplace(dir, entry, now()));
        }
    }

    // The entries of the archive, a header block of 512 bytes each, the
    // data of a file in the blocks after it. Names, targets and data are
    // the image's bytes.
    static void load(Server& s) {
        const u8* const archive = rootfs_image;
        const usize total = rootfs_image_size;
        usize at = 0;
        while (at + 512 <= total && archive[at] != 0) {
            const u8* block = archive + at;
            Name path = field(block, 100);
            const u32 perm = u32(octal(block + 100, 8)) & 0777;
            const u64 size = octal(block + 124, 12);
            const u8 kind = block[156];
            const Name target = field(block + 157, 100);
            if (size > total - at - 512) {
                fatal("rootfs: the image ends in an entry");
            }
            while (path.length && path.at[path.length - 1] == '/') {
                --path.length;
            }
            usize slash = path.length;
            while (slash > 0 && path.at[slash - 1] != '/') {
                --slash;
            }
            Node* dir = dirOf(s, {path.at, slash ? slash - 1 : 0});
            const Name base{path.at + slash, path.length - slash};
            switch (kind) {
            case '5':
                if (base.length) {
                    const Entry* entry = lookup(dir, base);
                    Node* n = entry ? entry->node : makeNode(s, dir, base, Kind::Dir, false);
                    n->perm = perm;
                }
                break;
            case '2': {
                unname(s, dir, base);
                Node* n = makeNode(s, dir, base, Kind::Link, false);
                n->content.view = target.at;
                n->content.viewSize = target.length;
                break;
            }
            default: {
                unname(s, dir, base);
                Node* file = makeNode(s, dir, base, Kind::File, false);
                fill(s, file, block + 512, usize(size));
                file->perm = perm;
            }
            }
            at += 512 + usize((size + 511) / 512 * 512);
        }
    }

    // The terminal is descriptor 0, from the kernel: the object itself goes
    // to whoever opens /dev/tty. urandom is the kernel's entropy, through
    // getentropy.
    static void devfs(Server& s) {
        s.writable = false;
        s.root = makeNode(s, nullptr, {}, Kind::Dir);
        makeNode(s, s.root, literal("tty"), Kind::Object, false)->fd = 0;
        makeNode(s, s.root, literal("null"), Kind::Null, false);
        makeNode(s, s.root, literal("zero"), Kind::Zero, false);
        makeNode(s, s.root, literal("urandom"), Kind::Random, false);
        makeNode(s, s.root, literal("random"), Kind::Random, false);
    }

    // A program run in a child whose root and working directory are root,
    // an end of a pair of ours: the child asks us, who serve when it does.
    static pid_t spawn(const char* path, char* const* argv, char* const* env, int root) {
        const pid_t pid = fork();
        if (pid == 0) {
            sigset_t blocked;
            if (waitMasked) {
                sigprocmask(SIG_SETMASK, &waitMask, &blocked);
            }
            if (fchdir(root) != 0 || chroot(".") != 0) {
                say("rootfs: chroot: errno ");
                sayNumber(errno);
                say("\n");
                _exit(127);
            }
            close(root);
            execve(path, argv, env);
            say("rootfs: ");
            say(path);
            say(": errno ");
            sayNumber(errno);
            say("\n");
            _exit(127);
        }
        if (pid < 0) {
            fatal("rootfs: fork");
        }
        return pid;
    }
}

int main() {
    image.writable = false;
    image.root = makeNode(image, nullptr, {}, Kind::Dir);
    load(image);
    devfs(devices);
    temporary.writable = true;
    temporary.capacity = tmpCapacity;
    temporary.root = makeNode(temporary, nullptr, {}, Kind::Dir);
    temporary.root->perm = 01777;
    mountOn(image, "dev", devices);
    mountOn(image, "tmp", temporary);
    int ends[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, ends) != 0) {
        fatal("socketpair");
    }
    // The shell's root is the far end of our own pair, never ours: a server
    // does not walk its own tree by path, and could not ask itself before
    // it serves.
    reapOnChild();
    char sh[] = "sh";
    char path[] = "PATH=/bin";
    char home[] = "HOME=/";
    char term[] = "TERM=vt100";
    char* const argv[] = {sh, nullptr};
    char* const env[] = {path, home, term, nullptr};
    shellPid = spawn("/bin/sh", argv, env, ends[1]);
    close(ends[1]);
    watcher = epoll_create1(EPOLL_CLOEXEC);
    if (watcher < 0) {
        fatal("fs: epoll_create1");
    }
    listen(image, ends[0], nullptr, true, false, nullptr, {});
    serve();
    return shellStatus > 0 ? shellStatus : 0;
}
