// rootfs: the system, the first process. It serves three file systems
// from one loop: the image linked into it, read-only, at the root; /dev,
// the terminal the kernel gave vm as descriptor 0, /dev/init, and the
// devices the services of the system register there; and /tmp, a tree in
// its memory of at most tmpCapacity bytes, a quarter of what a memory of
// wasm32 holds. It makes itself the root of everything else with what
// POSIX has: one seqpacket pair, chroot to one end in each process it
// starts. It starts the services of the system, /bin/devices first, and
// the shell on the terminal once they are up, each with a clean
// environment, reaps what ends under it, and ends with the shell's
// status, as a system does.
//
// A file server is a tree of nodes behind endpoints, which speaks the
// protocol of protocol.h. The kernel speaks it for the POSIX calls of a
// process (bin/vm/kernel_remote.go) and walks a path as Linux does:
// RESOLVE walks it here to the directory of its last name, and the call
// is one on that name, which the kernel follows itself when it is a
// symbolic link. /dev and /tmp are servers of this process mounted on
// names of the image (mountOn): a walk goes into the root of a server
// mounted on a name, and by ".." out of that root to where it is mounted.
//
// A server makes what it creates theirs who ask, and lets them do what
// its modes let them: root everything but executing what nobody may, the
// owner what the owner's bits say, the group of gid the group's, everyone
// else the others'. A path is searched through directories who asks may
// search, a name is made or removed in a directory they may write, and in
// a sticky one removed only by the owner of the name's node or of the
// directory.
//
// A device of /dev is a service's, which registered it at /dev/init: an
// OPEN of it goes on to the service as it came, its endpoint with it, and
// the service serves that open file from then on. rootfs keeps the name,
// its mode and its owner, and lets the name go when the service's end of
// the pair closes. The services rootfs starts get an end they close once
// their devices are there, and hand on to the services they start: when
// the last of them has closed it, the system is up.
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
// before the next: its names are read where they are in it. A file's bytes
// go straight to the memory of the process that reads them, or come
// straight from the memory of the one that writes them, through the memfd
// of it a READ or WRITE brings: one copy. A reply is sent from where its
// parts are: a header on the stack, a stat built there, a name of the
// request, a link's target. What a reply assembles, the entries of a
// directory, a path, goes into one buffer of its own, out. Nothing is
// allocated to answer a request; what a request makes is: a node, its
// name, a file's bytes, an endpoint. The image's files and names are its
// own bytes, never copied. The names of every directory are one
// open-addressing table, by directory and name; a directory keeps its
// entries in the order of their positions.

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/file.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "protocol.h"

using u8 = uint8_t;
using u32 = uint32_t;
using u64 = uint64_t;
using i64 = int64_t;
using usize = size_t;

// Placement new expressions are lowered directly by Clang; no runtime
// call uses this overload. Its declaration supplies the freestanding ABI.
void* operator new(size_t, void*) noexcept;

// The image: a ustar archive (bin/rootfs/image.py) linked in as data
// (bin/rootfs/make.py).
extern "C" const u8 rootfs_image[];
extern "C" const u32 rootfs_image_size;

namespace {
    constexpr usize maxPacket = 65536;

    // The servers'.
    constexpr u64 blockSize = 4096;
    constexpr u32 maxLinks = 40;  // the symbolic links one path goes through, Linux's
    constexpr u64 past = UINT64_MAX;  // the stop of a lock to the end of a file and past it
    constexpr u64 tmpCapacity = u64(1) << 30;

    [[noreturn, gnu::format(printf, 1, 2)]] static void fatal(const char* format, ...) {
        va_list args;
        va_start(args, format);
        vdprintf(STDERR_FILENO, format, args);
        va_end(args);
        dprintf(STDERR_FILENO, "\n");
        exit(1);
    }

    static usize checkedAdd(usize a, usize b) {
        if (b > SIZE_MAX - a) {
            fatal("rootfs: size overflow");
        }
        return a + b;
    }

    static usize checkedMultiply(usize a, usize b) {
        if (a != 0 && b > SIZE_MAX / a) {
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

    // ---- containers, bin/aot/aot.cpp's over malloc ------------------------------

    constexpr usize objectAlignment = 16;

    // An allocation's bytes: a power of two, from objectAlignment, as
    // aot.cpp's.
    static usize allocationBytes(usize size) {
        usize bytes = objectAlignment;
        while (bytes < size) {
            bytes = checkedMultiply(bytes, 2);
        }
        return bytes;
    }

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

// The capacity test stays small enough to be inlined where buffers grow
// element by element; growth itself is out of line.
inline void Buffer::reserve(usize size) {
    if (size > capacity()) {
        grow(size);
    }
}

[[gnu::noinline]] void Buffer::grow(usize size) {
    auto* before = header();
    const usize bytes = allocationBytes(checkedAdd(size, sizeof(Header)));
    Header* next;
    if (before == &emptyBuffer) {
        next = static_cast<Header*>(allocate(bytes));
        next->used = 0;
    } else {
        next = static_cast<Header*>(reallocate(before, bytes));
    }
    next->allocation = bytes;
    storage = next + 1;
}

inline void Buffer::resize(usize size) {
    reserve(size);
    if (size != 0 || header() != &emptyBuffer) {
        header()->used = size;
    }
}

void Buffer::append(const void* data, usize size) {
    const usize before = this->size();
    resize(checkedAdd(before, size));
    memcpy(static_cast<u8*>(storage) + before, data, size);
}

// Back to no bytes and no memory.
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
                if (!slots[i].item) {
                    return nullptr;
                }
                if (slots[i].hash == hash && same(slots[i].item)) {
                    return slots[i].item;
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
            // What follows moves back into the hole, unless that would put
            // it before its home: its home is in (i, j], cyclically.
            for (u32 j = (i + 1) & mask; slots[j].item; j = (j + 1) & mask) {
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
            slots = static_cast<Slot*>(calloc(capacity, sizeof(Slot)));
            if (!slots) {
                fatal("rootfs: out of memory");
            }
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

    // The bits of a 64-bit hash folded to 32, each output bit depending on
    // all (murmur3's finalizer).
    static u32 finish(u64 h) {
        h ^= h >> 33;
        h *= 0xff51afd7ed558ccdull;
        h ^= h >> 33;
        h *= 0xc4ceb9fe1a85ec53ull;
        h ^= h >> 33;
        return u32(h);
    }

    // ---- names and bodies ----------------------------------------------------------

    // Bytes of a packet, of the image or of a node, where they are.
    struct Name {
        const char* at = nullptr;
        usize length = 0;

        bool empty() const {
            return length == 0;
        }

        bool is(const char* text) const {
            return length == strlen(text) && memcmp(at, text, length) == 0;
        }

        bool has(char c) const {
            return length != 0 && memchr(at, c, length) != nullptr;
        }
    };

    static Name literal(const char* text) {
        return {text, strlen(text)};
    }

    // A request's body, read from its start.
    struct Body {
        const u8* at;
        const u8* end;

        usize size() const {
            return usize(end - at);
        }

        template <typename T>
        T take() {
            if (size() < sizeof(T)) {
                fatal("a short packet");
            }
            T value;
            memcpy(&value, at, sizeof value);
            at += sizeof value;
            return value;
        }

        // A string up to its NUL, or the end, and past the NUL.
        Name string() {
            const auto* nul = static_cast<const u8*>(memchr(at, 0, size()));
            const Name name{reinterpret_cast<const char*>(at), usize((nul ? nul : end) - at)};
            at = nul ? nul + 1 : end;
            return name;
        }

        Name rest() {
            const Name name{reinterpret_cast<const char*>(at), size()};
            at = end;
            return name;
        }
    };

    // ---- the tree --------------------------------------------------------------------

    struct Node;
    struct Server;

    enum class Kind : u8 {
        Dir,
        File,
        Link,
        Fifo,  // a named pipe: the kernel makes the pipe
        Object,  // an object of the kernel the server hands out: a tty
        Registry,  // /dev/init, where services register devices
        CharDevice,  // a service's device: the service serves its opens
        BlockDevice,
    };

    // A lock of fcntl: an endpoint's, on the bytes [start, stop), to read
    // or to write.
    struct Range {
        int owner;
        bool write;
        u64 start;
        u64 stop;
    };

    // A name of a directory: its bytes are the image's or follow the entry.
    struct Entry {
        Node* dir;
        Node* node;
        u64 pos;  // where the name is among the directory's entries
        Name name;
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

        Name name() const {
            return {reinterpret_cast<const char*>(data()), size()};
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
        int exclusive = -1;  // the endpoint that holds flock's exclusive lock, or -1
        int fd = -1;  // an object's; a device's end of its service's pair
        SmallVec<int> shared;  // the endpoints that hold flock's shared lock
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
        Server* s = nullptr;
        Node* node = nullptr;
    };

    struct Endpoint {
        int fd;
        bool reads;  // what its open lets it do
        bool writes;
        bool channel;  // a device's end of its service's pair: nothing comes on it
        Server* server;
        Node* node;  // what the endpoint is open on
        Node* dir;  // the directory a file was opened in, by name
        Endpoint* next;  // among the free ones
        u8 nameLength;
        char name[NAME_MAX];
    };

    static u32 hashIno(u64 ino) {
        return finish(ino);
    }

    Node* Server::find(u64 ino) const {
        return byIno.find(hashIno(ino), [ino](const Node* n) {
            return n->ino == ino;
        });
    }

    // FNV-1a over the directory and the name.
    static u32 hashName(const Node* dir, Name name) {
        u64 h = 0xcbf29ce484222325ull ^ u64(uintptr_t(dir));
        for (usize i = 0; i < name.length; ++i) {
            h = (h ^ u8(name.at[i])) * 0x100000001b3ull;
        }
        return finish(h);
    }

    // The names of all directories, by directory and name.
    OpenTable<Entry> names;

    static Entry* lookup(const Node* dir, Name name) {
        return names.find(hashName(dir, name), [dir, name](const Entry* e) {
            return e->dir == dir && e->name.length == name.length && memcmp(e->name.at, name.at, name.length) == 0;
        });
    }

    static u64 now() {
        timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        return u64(ts.tv_sec) * 1000000000 + u64(ts.tv_nsec);
    }

    static void touch(Node* n) {
        n->mtime = n->ctime = now();
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

    static void drop(Node* n) {
        if (--n->holds == 0 && dead(n)) {
            destroy(n);
        }
    }

    // Puts a name for a node in a directory, after every entry there: at
    // the next position. copy: the name's bytes go with the entry, else
    // they are the image's.
    static void place(Node* dir, Name name, Node* n, u64 t, bool copy = true) {
        auto* e = static_cast<Entry*>(allocate(checkedAdd(sizeof(Entry), copy ? name.length : 0)));
        if (copy) {
            char* bytes = reinterpret_cast<char*>(e + 1);
            memcpy(bytes, name.at, name.length);
            name.at = bytes;
        }
        *e = {dir, n, ++dir->entries, name, hashName(dir, name)};
        names.insert(e->hash, e);
        dir->order.pushBack({e->pos, e});
        ++dir->names;
        ++n->links;
        if (n->kind == Kind::Dir) {
            ++dir->subdirs;
            n->entry = e;
            if (n->parent != dir) {
                ++dir->holds;
                if (n->parent) {
                    drop(n->parent);
                }
                n->parent = dir;
            }
        }
        dir->mtime = dir->ctime = n->ctime = t;
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
        dir->mtime = dir->ctime = n->ctime = t;
        return n;
    }

    // A device for a server's files: the number of a pair, which no other
    // pair in the kernel has, as it never gives a number twice.
    static u64 device() {
        int ends[2];
        struct stat st;
        if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, ends) != 0 || fstat(ends[0], &st) != 0) {
            fatal("rootfs: a pair for a device: errno %d", errno);
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
        ++s.nodes;
        const u64 t = now();
        auto* n = new (allocate(sizeof(Node))) Node();
        n->ino = ++s.inodes;
        n->kind = kind;
        n->atime = n->mtime = n->ctime = t;
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

    // A file's size set, its bytes counted against the capacity, those past
    // what it had zeros: false when they do not fit, ENOSPC.
    static bool resize(Server& s, Node* n, u64 size) {
        Buffer& own = n->content.own;
        const u64 old = own.size();
        if ((size > old && s.capacity > 0 && s.used + (size - old) > s.capacity) || size > SIZE_MAX) {
            return false;
        }
        if (size == 0) {
            own.clear();
        } else {
            own.resize(usize(size));
            if (size > old) {
                memset(static_cast<u8*>(own.data()) + old, 0, usize(size - old));
            }
        }
        s.used = s.used + size - old;
        return true;
    }

    // Whether a node is a service's device.
    static bool isDevice(const Node* n) {
        return n->kind == Kind::CharDevice || n->kind == Kind::BlockDevice;
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
        return (dir->perm & S_ISVTX) != 0 && !owns(n, h) && h.uid != dir->uid;
    }

    static u32 modeOf(const Node* n) {
        switch (n->kind) {
        case Kind::Dir:
            return S_IFDIR | n->perm;
        case Kind::File:
            return S_IFREG | n->perm;
        case Kind::Link:
            return S_IFLNK | n->perm;
        case Kind::Fifo:
            return S_IFIFO | n->perm;
        case Kind::BlockDevice:
            return S_IFBLK | n->perm;
        default:
            return S_IFCHR | n->perm;
        }
    }

    static u8 dtype(const Node* n) {
        switch (n->kind) {
        case Kind::Dir:
            return DT_DIR;
        case Kind::File:
            return DT_REG;
        case Kind::Link:
            return DT_LNK;
        case Kind::Fifo:
            return DT_FIFO;
        case Kind::BlockDevice:
            return DT_BLK;
        default:
            return DT_CHR;
        }
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
        return {};
    }

    // Where a name goes: the root of a server mounted on it, or its node.
    static Place down(Place p) {
        while (mountPoint(p.node)) {
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
    // names with "/" are a walk's; a name past NAME_MAX is too long.
    static int badName(Name name) {
        if (name.is(".") || name.is("..") || name.has('/')) {
            return EINVAL;
        }
        if (name.length > NAME_MAX) {
            return ENAMETOOLONG;
        }
        return 0;
    }

    // What a name of a directory is to a call that looks at it: the
    // directory itself for "", else the name's node, or the root of a
    // server mounted on it; 0, or why there is none.
    static int named(Server* s, Node* c, Name name, const Header& h, Place& out) {
        if (name.empty()) {
            out = {s, c};
            return 0;
        }
        if (const int e = badName(name)) {
            return e;
        }
        if (c->kind != Kind::Dir) {
            return ENOTDIR;
        }
        if (!may(c, h, 1)) {
            return EACCES;
        }
        const Entry* entry = lookup(c, name);
        if (!entry) {
            return ENOENT;
        }
        out = down({s, entry->node});
        return 0;
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

    // A directory's links: its name, its ".", and the ".." of each
    // directory in it; another's: its names.
    static Stat statOf(const Server& s, const Node* n) {
        Stat st{};
        st.ino = n->ino;
        st.mode = modeOf(n);
        st.nlink = n->kind == Kind::Dir ? 2 + n->subdirs : n->links;
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
        st.namemax = NAME_MAX;
        st.dev = s.dev;
        const u64 used = (s.used + blockSize - 1) / blockSize;
        if (s.writable && s.capacity > 0) {
            st.blocks = st.files = s.capacity / blockSize;
            st.bfree = st.blocks - MIN(used, st.blocks);
            st.ffree = st.files - MIN(s.nodes, st.files);
        } else {
            st.blocks = used;
            st.files = s.nodes;
        }
        if (!s.writable) {
            st.flags = ST_RDONLY;
        }
        return st;
    }

    // ---- packets ---------------------------------------------------------------------

    // The packet recv takes and the descriptors that come with it, and the
    // bytes a reply assembles: one of each for the process, whose loop
    // takes a packet and is done with it before the next, and sends each
    // reply whole.
    alignas(Header) u8 packet[maxPacket];
    int packetFds[maxFds];
    usize packetFdCount;
    u8 out[maxPacket - sizeof(Header)];

    // A reply: the header, then the parts where they are.
    static void reply(int fd, u32 op, u32 arg, const iovec* parts, usize count, const int* fds = nullptr, usize fdCount = 0) {
        Header head{op, arg, 0, 0};
        iovec iov[3] = {{&head, sizeof head}};
        if (count > 2 || fdCount > maxFds) {
            fatal("rootfs: a reply of %zu parts and %zu descriptors", count, fdCount);
        }
        for (usize i = 0; i < count; ++i) {
            iov[1 + i] = parts[i];
        }
        sendPacket(fd, iov, 1 + count, fds, fdCount);  // a gone reader is its own loss
    }

    static iovec part(const void* data, usize size) {
        return {const_cast<void*>(data), size};
    }

    static void ok(int fd, const void* data = nullptr, usize size = 0) {
        const iovec body = part(data, size);
        reply(fd, opOk, 0, &body, size ? 1 : 0);
    }

    // A reply of an error: the reply's endpoint is not kept.
    static bool error(int fd, int e) {
        reply(fd, opErr, u32(e), nullptr, 0);
        return false;
    }

    // The memfd of the memory a READ fills or a WRITE takes from: the
    // first of the request's descriptors, before its reply endpoint; -1
    // when it brought none.
    static int window() {
        return packetFdCount >= 2 ? packetFds[0] : -1;
    }

    // Puts count bytes into a READ's window at at: false when they do not
    // all go.
    static bool put(int w, const void* from, usize count, usize at) {
        return count == 0 || pwrite(w, from, count, off_t(at)) == ssize_t(count);
    }

    // The reply of a READ: how many bytes went into its window.
    static bool readCount(int fd, usize count) {
        const u32 n = u32(count);
        ok(fd, &n, sizeof n);
        return false;
    }

    // The reply of a call that changes something: OK, or the error.
    static bool answer(int fd, int e) {
        if (e != 0) {
            return error(fd, e);
        }
        ok(fd);
        return false;
    }

    // One packet into packet, its descriptors into packetFds: the length
    // of its body, after the header, or -1 at the end of the endpoint.
    static i64 receive(int fd, Header& h) {
        const ssize_t n = receivePacket(fd, packet, sizeof packet, packetFds, packetFdCount);
        if (n <= 0) {
            return -1;
        }
        if (usize(n) < sizeof(Header)) {
            fatal("a packet without a header");
        }
        memcpy(&h, packet, sizeof h);
        return i64(n) - i64(sizeof(Header));
    }

    // What a reply assembles in out.
    struct Assembly {
        usize size = 0;

        bool fits(usize n) const {
            return n <= sizeof out - size;
        }

        void add(const void* data, usize n) {
            memcpy(out + size, data, n);
            size += n;
        }

        template <typename T>
        void add(T value) {
            add(&value, sizeof value);
        }
    };

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
    // read, to write or both; a file opened by a name of dir says it. A
    // channel is a device's end of its service's pair, by its name in dir.
    // Its descriptor came with MSG_CMSG_CLOEXEC, or from a pair of ours,
    // made with SOCK_CLOEXEC.
    static void listen(Server& s, int fd, Node* node, bool reads, bool writes, Node* dir, Name name, bool channel = false) {
        Node* n = node ? node : s.root;
        Endpoint* ep = freeEndpoints;
        if (ep) {
            freeEndpoints = ep->next;
        } else {
            ep = static_cast<Endpoint*>(allocate(sizeof(Endpoint)));
        }
        if (name.length > NAME_MAX) {
            fatal("rootfs: a name of %zu bytes for an endpoint", name.length);
        }
        *ep = {fd, reads, writes, channel, &s, n, dir, nullptr, u8(name.length), {}};
        if (!name.empty()) {
            memcpy(ep->name, name.at, name.length);
        }
        ++n->opens;
        if (dir) {
            ++dir->holds;
        }
        ++endpointCount;
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.ptr = ep;
        if (epoll_ctl(watcher, EPOLL_CTL_ADD, fd, &ev) != 0) {
            fatal("fs: epoll_ctl: errno %d", errno);
        }
    }

    // A device's end of its service's pair closed: the service is gone,
    // and the device's name with it. What is open of the device stays the
    // service's, gone too.
    static void unregister(Endpoint* ep) {
        Node* n = ep->node;
        if (Entry* entry = lookup(ep->dir, {ep->name, ep->nameLength}); entry && entry->node == n) {
            unplace(ep->dir, entry, now());
        }
        n->fd = -1;
    }

    // An endpoint that closes: its locks go with it, and its node when no
    // name is left of it.
    static void forget(Endpoint* ep) {
        Node* n = ep->node;
        const int fd = ep->fd;
        if (ep->channel) {
            unregister(ep);
        }
        n->shared.keepIf([fd](int holder) {
            return holder != fd;
        });
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
    //
    // Each takes the reply's endpoint and says whether it is kept: an
    // endpoint of the server now, which the loop does not close.

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

        static Name trimmed(Name n) {
            while (n.length && *n.at == '/') {
                ++n.at;
                --n.length;
            }
            return n;
        }

        // The next name, or false at the end.
        bool take(Name& name) {
            for (; depth > 0; --depth) {
                Name& top = stack[depth - 1];
                top = trimmed(top);
                if (!top.empty()) {
                    const auto* slash = static_cast<const char*>(memchr(top.at, '/', top.length));
                    name = {top.at, slash ? usize(slash - top.at) : top.length};
                    top.at += name.length;
                    top.length -= name.length;
                    return true;
                }
            }
            return false;
        }

        // Whether a name is left.
        bool more() const {
            for (u32 i = 0; i < depth; ++i) {
                if (!trimmed(stack[i]).empty()) {
                    return true;
                }
            }
            return false;
        }
    };

    // A walk to a root of no server of ours: the kernel goes on from the
    // root of the process with the names left.
    static bool fromRoot(int fd, u32 links, Walk& walk, bool slash) {
        Assembly a;
        a.add(stepRoot);
        a.add(links);
        a.add(lastNone);
        a.add(u32(0));
        Name left;
        for (bool first = true; walk.take(left); first = false) {
            if (!a.fits(left.length + 2)) {
                return error(fd, ENAMETOOLONG);
            }
            if (!first) {
                a.add("/", 1);
            }
            a.add(left.at, left.length);
        }
        if (slash) {
            a.add("/", 1);
        }
        ok(fd, out, a.size);
        return false;
    }

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
    static bool resolve(Endpoint* ep, const Header& h, Body body, int fd) {
        const u64 dev = body.take<u64>();
        const u64 ino = body.take<u64>();
        u32 links = body.take<u32>();
        const Name path = body.rest();
        const bool slash = !path.empty() && path.at[path.length - 1] == '/';
        Walk walk;
        walk.push(path);
        Place here{ep->server, ep->node};
        Name name;
        bool have = walk.take(name);
        for (; have && walk.more(); have = walk.take(name)) {
            if (here.node->kind != Kind::Dir) {
                return error(fd, ENOTDIR);
            }
            if (!may(here.node, h, 1)) {
                return error(fd, EACCES);
            }
            if (name.is(".")) {
                continue;
            }
            if (name.is("..")) {
                here = up(here, dev, ino);
                continue;
            }
            if (name.length > NAME_MAX) {
                return error(fd, ENAMETOOLONG);
            }
            const Entry* entry = lookup(here.node, name);
            if (!entry) {
                return error(fd, ENOENT);
            }
            const Place next = down({here.s, entry->node});
            if (next.node->kind != Kind::Link) {
                here = next;
                continue;
            }
            if (links >= maxLinks) {
                return error(fd, ELOOP);
            }
            const Name target = next.node->content.name();
            if (target.empty()) {
                return error(fd, ENOENT);
            }
            ++links;
            walk.push(target);
            if (target.at[0] == '/') {
                here = found(dev, ino);
                if (!here.node) {
                    return fromRoot(fd, links, walk, slash);
                }
            }
        }
        if (here.node->kind != Kind::Dir) {
            return error(fd, ENOTDIR);
        }
        u32 last = lastNone;
        Name final;
        if (have) {
            if (!may(here.node, h, 1)) {
                return error(fd, EACCES);
            }
            if (name.is(".")) {
                last = lastDot;
            } else if (name.is("..")) {
                last = lastDotdot;
                here = up(here, dev, ino);
            } else if (name.length > NAME_MAX) {
                return error(fd, ENAMETOOLONG);
            } else {
                last = lastName;
                final = name;
            }
        }
        const bool kept = here.s != ep->server || here.node != ep->node;
        const u32 head[4] = {stepDone, links, last, (slash && last == lastName ? stepSlash : 0) | (kept ? stepKept : 0)};
        const iovec parts[] = {part(head, sizeof head), part(final.at, final.length)};
        if (kept) {
            listen(*here.s, fd, here.node, false, false, nullptr, {});
        }
        reply(fd, opOk, 0, parts, final.empty() ? 1 : 2);
        return kept;
    }

    // The path of an endpoint's directory from the root of the process,
    // the node of a device and inode, up through the places ".." goes to;
    // of a file, its directory's and the name it was opened by. A removed
    // directory has none, nor one the root is not above: ENOENT. The path
    // is made from its end, at the end of out.
    static bool pathOf(Endpoint* ep, Body body, int fd) {
        const u64 dev = body.take<u64>();
        const u64 ino = body.take<u64>();
        Place here{ep->server, ep->node};
        usize start = sizeof out;
        auto prepend = [&start](Name name) {
            if (start < name.length + 1) {
                return false;
            }
            start -= name.length;
            memcpy(out + start, name.at, name.length);
            out[--start] = '/';
            return true;
        };
        if (here.node->kind != Kind::Dir) {
            if (!ep->dir) {
                return error(fd, ENOENT);
            }
            if (!prepend({ep->name, ep->nameLength})) {
                return error(fd, ENAMETOOLONG);
            }
            here = {ep->server, ep->dir};
        }
        while (!at(here, dev, ino)) {
            if (here.node == here.s->root) {
                if (!here.s->above) {
                    return error(fd, ENOENT);
                }
                here = {here.s->aboveServer, here.s->above};
                continue;
            }
            const Entry* entry = here.node->entry;
            if (gone(here.s, here.node) || !entry || entry->name.empty()) {
                return error(fd, ENOENT);
            }
            if (!prepend(entry->name)) {
                return error(fd, ENAMETOOLONG);
            }
            here = {here.s, here.node->parent};
        }
        if (start == sizeof out) {
            ok(fd, "/", 1);
        } else {
            ok(fd, out + start, sizeof out - start);
        }
        return false;
    }

    // An OPEN of a device goes on to the service that registered it as it
    // came, its endpoint with it: the service answers on the endpoint and
    // serves the open file from then on. ENXIO when the service is gone,
    // EAGAIN when it does not keep up with its opens.
    static bool forward(const Node* n, Body body, int fd) {
        const iovec all = part(packet, usize(body.end - packet));
        if (n->fd < 0 || sendPacket(n->fd, &all, 1, &fd, 1, MSG_DONTWAIT) < 0) {
            return error(fd, n->fd >= 0 && errno == EAGAIN ? EAGAIN : ENXIO);
        }
        return false;
    }

    // A name of the directory opened, or the directory itself for "": read
    // or written as its access mode says and who asks may, made when
    // O_CREAT asks and it is not there, emptied when O_TRUNC asks; with
    // O_PATH only held, for a working directory or a root. A symbolic link
    // is ELOOP, for the kernel to follow, or held itself with O_PATH and
    // O_NOFOLLOW.
    static bool openName(Endpoint* ep, const Header& h, Body body, int fd) {
        Server* s = ep->server;
        Node* c = ep->node;
        const u32 mode = body.take<u32>();
        const Name name = body.string();
        const u32 flags = h.arg;
        const bool opath = (flags & O_PATH) != 0;
        const u32 access = flags & O_ACCMODE;
        const bool reads = !opath && access != O_WRONLY;
        const bool writes = !opath && access != O_RDONLY;
        const bool truncates = !opath && (flags & O_TRUNC) != 0;
        const bool creating = (flags & O_CREAT) != 0 && !name.empty();
        const bool there = !name.empty() && c->kind == Kind::Dir && lookup(c, name);
        Place target;
        bool made = false;
        if (creating && there && (flags & O_EXCL) != 0) {
            return error(fd, EEXIST);
        }
        if (creating && !there) {
            if (const int e = creates(s, c, name, h)) {
                return error(fd, e);
            }
            Node* n = makeNode(*s, c, name, Kind::File);
            n->perm = mode & 07777;
            n->uid = h.uid;
            n->gid = h.gid;
            target = {s, n};
            made = true;
        } else if (const int e = named(s, c, name, h, target)) {
            return error(fd, e);
        }
        Node* n = target.node;
        if (n->kind == Kind::Link && !(opath && (flags & O_NOFOLLOW) != 0)) {
            return error(fd, ELOOP);
        }
        if ((flags & O_DIRECTORY) != 0 && n->kind != Kind::Dir) {
            return error(fd, ENOTDIR);
        }
        if (!made && !opath) {
            if (!may(n, h, (reads ? 4 : 0) | (writes || truncates ? 2 : 0))) {
                return error(fd, EACCES);
            }
            if (n->kind == Kind::Dir && (writes || (flags & O_CREAT) != 0)) {
                return error(fd, EISDIR);
            }
            if ((writes || truncates) && n->kind == Kind::File && !target.s->writable) {
                return error(fd, EROFS);
            }
        }
        if (n->kind == Kind::Object && !opath) {
            reply(fd, opOk, 0, nullptr, 0, &n->fd, 1);  // the object itself becomes the caller's
            return false;
        }
        if (isDevice(n) && !opath) {
            return forward(n, body, fd);
        }
        if (truncates && n->kind == Kind::File && n->content.size() > 0) {
            resize(*target.s, n, 0);
            touch(n);
        }
        const Stat st = statOf(*target.s, n);
        listen(*target.s, fd, n, reads, writes, name.empty() ? nullptr : c, name);
        ok(fd, &st, sizeof st);
        return true;
    }

    static bool statName(Endpoint* ep, const Header& h, Body body, int fd) {
        Place t;
        if (const int e = named(ep->server, ep->node, body.string(), h, t)) {
            return error(fd, e);
        }
        const Stat st = statOf(*t.s, t.node);
        ok(fd, &st, sizeof st);
        return false;
    }

    // The file system of a name: the server's, or the one mounted there.
    static bool statfsName(Endpoint* ep, const Header& h, Body body, int fd) {
        Place t;
        if (const int e = named(ep->server, ep->node, body.string(), h, t)) {
            return error(fd, e);
        }
        const Statfs st = statfsOf(*t.s);
        ok(fd, &st, sizeof st);
        return false;
    }

    static bool readlinkName(Endpoint* ep, const Header& h, Body body, int fd) {
        Place t;
        if (const int e = named(ep->server, ep->node, body.string(), h, t)) {
            return error(fd, e);
        }
        if (t.node->kind != Kind::Link) {
            return error(fd, EINVAL);
        }
        ok(fd, t.node->content.data(), t.node->content.size());
        return false;
    }

    // mkdir, mknod and symlink: a new name in the directory, who asks may
    // write, theirs, with the mode asked for. mknod makes FIFOs and regular
    // files; a device is EPERM.
    static bool create(Endpoint* ep, const Header& h, Body body, int fd) {
        const Name target = h.op == opSymlink ? body.string() : Name{};
        const Name name = body.string();
        if (const int e = creates(ep->server, ep->node, name, h)) {
            return error(fd, e);
        }
        Kind kind = Kind::Dir;
        if (h.op == opSymlink) {
            kind = Kind::Link;
        } else if (h.op == opMknod) {
            switch (h.arg & S_IFMT) {
            case S_IFIFO:
                kind = Kind::Fifo;
                break;
            case 0:
            case S_IFREG:
                kind = Kind::File;
                break;
            default:
                return error(fd, EPERM);
            }
        }
        Node* n = makeNode(*ep->server, ep->node, name, kind);
        n->uid = h.uid;
        n->gid = h.gid;
        if (kind == Kind::Link) {
            n->content.own.append(target.at, target.length);
        } else {
            n->perm = h.arg & 07777;
        }
        return answer(fd, 0);
    }

    static int unlinkError(Server* s, Node* c, Name name, bool removeDir, const Header& h) {
        if (const int e = badName(name)) {
            return e;
        }
        if (c->kind != Kind::Dir) {
            return ENOTDIR;
        }
        if (!may(c, h, 1)) {
            return EACCES;
        }
        if (!s->writable) {
            return EROFS;
        }
        Entry* entry = lookup(c, name);
        if (!entry) {
            return ENOENT;
        }
        const Node* n = entry->node;
        if (removeDir && n->kind != Kind::Dir) {
            return ENOTDIR;
        }
        if (!removeDir && n->kind == Kind::Dir) {
            return EISDIR;
        }
        if (!may(c, h, 3)) {
            return EACCES;
        }
        if (keeps(c, n, h)) {
            return EPERM;
        }
        if (removeDir && mountPoint(n)) {
            return EBUSY;
        }
        if (removeDir && n->names > 0) {
            return ENOTEMPTY;
        }
        release(*s, unplace(c, entry, now()));
        return 0;
    }

    static bool unlinkName(Endpoint* ep, const Header& h, Body body, int fd) {
        return answer(fd, unlinkError(ep->server, ep->node, body.string(), (h.arg & AT_REMOVEDIR) != 0, h));
    }

    // Whether dir is n or under it.
    static bool within(const Node* n, const Node* dir) {
        for (; dir; dir = dir->parent) {
            if (dir == n) {
                return true;
            }
        }
        return false;
    }

    // The directory of a device and inode another name of a call is in:
    // EXDEV on another server, ENOENT when it is no directory there now.
    static int directory(const Server* s, u64 dev, u64 ino, Node*& dir) {
        if (dev != s->dev) {
            return EXDEV;
        }
        dir = s->find(ino);
        if (!dir || dir->kind != Kind::Dir || gone(s, dir)) {
            return ENOENT;
        }
        return 0;
    }

    // A name of the directory moved to a name of another of the server's,
    // of a device and inode: on another server EXDEV. What the new name
    // names goes, when it is of the old one's kind, and empty if a
    // directory; a directory does not go under itself, nor a mount point
    // anywhere, and one that goes to another directory must be one who
    // asks may write, for its "..".
    static int renameError(Server* s, Node* src, const Header& h, Body body) {
        const u64 dev = body.take<u64>();
        const u64 ino = body.take<u64>();
        const Name oldName = body.string();
        const Name newName = body.string();
        Node* dst = nullptr;
        if (const int e = badName(oldName)) {
            return e;
        }
        if (const int e = badName(newName)) {
            return e;
        }
        if (const int e = directory(s, dev, ino, dst)) {
            return e;
        }
        if (src->kind != Kind::Dir) {
            return ENOTDIR;
        }
        if (!may(src, h, 1) || !may(dst, h, 1)) {
            return EACCES;
        }
        if (!s->writable) {
            return EROFS;
        }
        Entry* entry = lookup(src, oldName);
        if (!entry) {
            return ENOENT;
        }
        Node* n = entry->node;
        Entry* replaced = lookup(dst, newName);
        const Node* old = replaced ? replaced->node : nullptr;
        if (old == n) {
            return 0;
        }
        if (n->kind == Kind::Dir && within(n, dst)) {
            return EINVAL;
        }
        if (!may(src, h, 3) || !may(dst, h, 3)) {
            return EACCES;
        }
        if (keeps(src, n, h) || (old && keeps(dst, old, h))) {
            return EPERM;
        }
        if (n->kind == Kind::Dir && src != dst && !may(n, h, 2)) {
            return EACCES;
        }
        if (mountPoint(n) || (old && mountPoint(old))) {
            return EBUSY;
        }
        if (old && n->kind == Kind::Dir && old->kind != Kind::Dir) {
            return ENOTDIR;
        }
        if (old && n->kind != Kind::Dir && old->kind == Kind::Dir) {
            return EISDIR;
        }
        if (old && old->kind == Kind::Dir && old->names > 0) {
            return ENOTEMPTY;
        }
        const u64 t = now();
        unplace(src, entry, t);
        if (replaced) {
            release(*s, unplace(dst, replaced, t));
        }
        place(dst, newName, n, t);
        return 0;
    }

    static bool renameName(Endpoint* ep, const Header& h, Body body, int fd) {
        return answer(fd, renameError(ep->server, ep->node, h, body));
    }

    // A new name, in a directory of the server's of a device and inode, for
    // the node a name of this one is, which is no directory.
    static int linkError(Server* s, Node* src, const Header& h, Body body) {
        const u64 dev = body.take<u64>();
        const u64 ino = body.take<u64>();
        const Name oldName = body.string();
        const Name newName = body.string();
        if (const int e = badName(oldName)) {
            return e;
        }
        if (src->kind != Kind::Dir) {
            return ENOTDIR;
        }
        if (!may(src, h, 1)) {
            return EACCES;
        }
        const Entry* entry = lookup(src, oldName);
        if (!entry) {
            return ENOENT;
        }
        Node* dst = nullptr;
        if (const int e = directory(s, dev, ino, dst)) {
            return e;
        }
        if (const int e = creates(s, dst, newName, h)) {
            return e;
        }
        if (entry->node->kind == Kind::Dir) {
            return EPERM;
        }
        place(dst, newName, entry->node, now());
        return 0;
    }

    static bool linkName(Endpoint* ep, const Header& h, Body body, int fd) {
        return answer(fd, linkError(ep->server, ep->node, h, body));
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
            const u32 uid = args.take<u32>();
            const u32 gid = args.take<u32>();
            const u32 toUid = uid == noId ? node->uid : uid;
            const u32 toGid = gid == noId ? node->gid : gid;
            if (h.uid != 0 && (h.uid != node->uid || toUid != node->uid || (toGid != node->gid && toGid != h.gid))) {
                return EPERM;
            }
            if ((toUid != node->uid || toGid != node->gid) && node->kind != Kind::Dir) {
                node->perm &= ~u32(S_ISUID | S_ISGID);
            }
            node->uid = toUid;
            node->gid = toGid;
            break;
        }
        case opUtimes:
        case opFutimes: {
            const u64 atime = args.take<u64>();
            const u64 mtime = args.take<u64>();
            const bool toNow = (h.arg & whenAtimeNow) != 0 && (h.arg & whenMtimeNow) != 0;
            if (toNow && !owns(node, h) && !may(node, h, 2)) {
                return EACCES;
            }
            if (!toNow && !owns(node, h)) {
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
            const u64 size = args.take<u64>();
            if (node->kind == Kind::Dir) {
                return op == opTruncate ? EISDIR : EINVAL;
            }
            if (node->kind != Kind::File || size > u64(INT32_MAX) * 1024) {
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
    static bool attr(Endpoint* ep, const Header& h, Body body, int fd) {
        const usize ahead = h.op == opChown || h.op == opTruncate ? 8 : h.op == opUtimes ? 16 : 0;
        if (body.size() < ahead) {
            fatal("a short packet");
        }
        const Body args{body.at, body.at + ahead};
        Body rest{body.at + ahead, body.end};
        Place t;
        if (const int e = named(ep->server, ep->node, rest.string(), h, t)) {
            return error(fd, e);
        }
        if (!t.s->writable) {
            return error(fd, EROFS);
        }
        return answer(fd, change(*t.s, t.node, h.op, h, args));
    }

    // fchmod, fchown, futimes, ftruncate and fsync of an open node; only an
    // endpoint open to write truncates.
    static bool fattr(Endpoint* ep, const Header& h, Body body, int fd) {
        if (h.op == opFsync) {
            return answer(fd, 0);
        }
        if (!ep->server->writable) {
            return error(fd, EROFS);
        }
        if (h.op == opFtruncate && !ep->writes) {
            return error(fd, EINVAL);
        }
        return answer(fd, change(*ep->server, ep->node, h.op, h, body));
    }

    // ---- calls on a node ---------------------------------------------------------

    static bool readNode(Endpoint* ep, const Header& h, Body body, int fd) {
        const u64 off = body.take<u64>();
        const u32 woff = body.take<u32>();
        const Node* node = ep->node;
        const usize n = h.arg;
        if (off > INT64_MAX) {
            return error(fd, EINVAL);
        }
        if (node->kind != Kind::Dir && !ep->reads) {
            return error(fd, EBADF);
        }
        const int w = window();
        if (w < 0 && node->kind != Kind::Dir) {
            return error(fd, EINVAL);
        }
        switch (node->kind) {
        case Kind::File: {
            const usize size = node->content.size();
            const usize count = off >= size ? 0 : MIN(n, size - usize(off));
            if (!put(w, node->content.data() + (count ? usize(off) : 0), count, woff)) {
                return error(fd, EIO);
            }
            return readCount(fd, count);
        }
        default:
            return error(fd, node->kind == Kind::Dir ? EISDIR : EINVAL);
        }
    }

    // The data at the offset, or at the end with append, as much as the
    // capacity takes: the count and where it went.
    static bool writeNode(Endpoint* ep, const Header& h, Body body, int fd) {
        u64 off = body.take<u64>();
        const usize length = body.take<u32>();
        const u32 woff = body.take<u32>();
        Server& s = *ep->server;
        Node* node = ep->node;
        const int w = window();
        if (off > INT64_MAX) {
            return error(fd, EINVAL);
        }
        if (node->kind != Kind::Dir && !ep->writes) {
            return error(fd, EBADF);
        }
        if (w < 0 && node->kind != Kind::Dir) {
            return error(fd, EINVAL);
        }
        u64 count = length;
        switch (node->kind) {
        case Kind::File: {
            if (!s.writable) {
                return error(fd, EROFS);
            }
            const u64 size = node->content.size();
            if ((h.arg & 1) != 0) {
                off = size;
            }
            const u64 grows = off + count > size ? off + count - size : 0;
            if (s.capacity > 0 && grows > s.capacity - s.used) {
                const u64 over = grows - (s.capacity - s.used);
                count = count > over ? count - over : 0;
            }
            if (count == 0 && length > 0) {
                return error(fd, ENOSPC);
            }
            if (size < off + count && !resize(s, node, off + count)) {
                return error(fd, ENOSPC);
            }
            if (count && pread(w, static_cast<u8*>(node->content.own.data()) + usize(off), usize(count), woff) != ssize_t(count)) {
                return error(fd, EIO);
            }
            touch(node);
            break;
        }
        default:
            return error(fd, node->kind == Kind::Dir ? EISDIR : EINVAL);
        }
        Assembly a;
        a.add(u32(count));
        a.add(off);
        ok(fd, out, a.size);
        return false;
    }

    // The entries after a position, in order, as many as one reply holds.
    static bool readdirNode(const Node* node, Body body, int fd) {
        if (node->kind != Kind::Dir) {
            return error(fd, ENOTDIR);
        }
        const u64 after = body.take<u64>();
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
        Assembly a;
        for (usize i = lo; i < count; ++i) {
            const Entry* e = slots[i].entry;
            if (!e) {
                continue;
            }
            if (!a.fits(17 + e->name.length + 1)) {
                break;
            }
            a.add(e->node->ino);
            a.add(e->pos);
            a.add(dtype(e->node));
            a.add(e->name.at, e->name.length);
            a.add(u8(0));
        }
        ok(fd, out, a.size);
        return false;
    }

    // flock's lock of the endpoint's node: shared, which no other
    // endpoint's exclusive one is in the way of, exclusive, which no other
    // endpoint's lock is, or none.
    static bool flockNode(Endpoint* ep, const Header& h, int fd) {
        Node* n = ep->node;
        const int me = ep->fd;
        const bool other = n->exclusive >= 0 && n->exclusive != me;
        usize others = 0;
        for (usize i = 0; i < n->shared.size(); ++i) {
            others += n->shared[i] != me;
        }
        auto unshare = [n, me] {
            n->shared.keepIf([me](int holder) {
                return holder != me;
            });
        };
        switch (h.arg & ~u32(LOCK_NB)) {
        case LOCK_UN:
            unshare();
            if (n->exclusive == me) {
                n->exclusive = -1;
            }
            break;
        case LOCK_SH:
            if (other) {
                return error(fd, EAGAIN);
            }
            n->exclusive = -1;
            unshare();
            n->shared.pushBack(me);
            break;
        case LOCK_EX:
            if (other || others > 0) {
                return error(fd, EAGAIN);
            }
            unshare();
            n->exclusive = me;
            break;
        default:
            return error(fd, EINVAL);
        }
        return answer(fd, 0);
    }

    // The ranges a LOCK keeps, made here and swapped in: one of each for
    // the process.
    SmallVec<Range> keptRanges;

    // fcntl's locks of the endpoint's node, on a range, to the end and past
    // with a length of 0. With get, the first lock of another endpoint in
    // the way of the one described, else F_UNLCK. Else that lock is set in
    // place of what the endpoint held of the range, unless another
    // endpoint's lock is in the way: EAGAIN; F_UNLCK lets the range go.
    static bool lockNode(Endpoint* ep, const Header& h, Body body, int fd) {
        const u32 type = body.take<u32>();
        const u64 start = body.take<u64>();
        const u64 length = body.take<u64>();
        const u64 stop = length == 0 || start > past - length ? past : start + length;
        if (type > F_UNLCK) {
            return error(fd, EINVAL);
        }
        Node* n = ep->node;
        const int me = ep->fd;
        const Range* way = nullptr;
        for (usize i = 0; i < n->ranges.size() && !way; ++i) {
            const Range& l = n->ranges[i];
            if (l.owner != me && l.start < stop && start < l.stop && (l.write || type == F_WRLCK)) {
                way = &l;
            }
        }
        if (h.arg == 1) {
            Assembly a;
            if (way) {
                a.add(u32(way->write ? F_WRLCK : F_RDLCK));
                a.add(way->start);
                a.add(way->stop == past ? 0 : way->stop - way->start);
            } else {
                a.add(u32(F_UNLCK));
                a.add(start);
                a.add(length);
            }
            ok(fd, out, a.size);
            return false;
        }
        if (type != F_UNLCK && way) {
            return error(fd, EAGAIN);
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
        if (type != F_UNLCK) {
            keptRanges.pushBack({me, type == F_WRLCK, start, stop});
        }
        n->ranges.buffer.swap(keptRanges.buffer);
        return answer(fd, 0);
    }

    // ---- devices ----------------------------------------------------------------

    // A service's device: a name in /dev of the mode asked for, S_IFCHR or
    // S_IFBLK, whose opens go on to the end of a pair the request brings,
    // the service's the other; its stat is the reply. Only on an endpoint
    // of /dev/init open to write.
    static bool registerName(Endpoint* ep, const Header& h, Body body, int fd) {
        const Name name = body.string();
        const u32 type = h.arg & S_IFMT;
        Server& s = devices;
        if (ep->node->kind != Kind::Registry || !ep->writes) {
            return error(fd, EBADF);
        }
        if (packetFdCount != 2 || (type != S_IFCHR && type != S_IFBLK) || name.empty()) {
            return error(fd, EINVAL);
        }
        if (const int e = badName(name)) {
            return error(fd, e);
        }
        if (lookup(s.root, name)) {
            return error(fd, EEXIST);
        }
        Node* n = makeNode(s, s.root, name, type == S_IFBLK ? Kind::BlockDevice : Kind::CharDevice);
        n->perm = h.arg & 07777;
        n->uid = h.uid;
        n->gid = h.gid;
        n->fd = packetFds[0];
        packetFds[0] = -1;  // the device's now: handle leaves it open
        listen(s, n->fd, n, false, false, s.root, name, true);
        const Stat st = statOf(s, n);
        ok(fd, &st, sizeof st);
        return false;
    }

    // ---- the loop ---------------------------------------------------------------

    // A request on an endpoint, answered on fd: whether fd is kept.
    static bool dispatch(Endpoint* ep, const Header& h, Body body, int fd) {
        switch (h.op) {
        case opResolve:
            return resolve(ep, h, body, fd);
        case opPath:
            return pathOf(ep, body, fd);
        case opOpen:
            return openName(ep, h, body, fd);
        case opStat:
            return statName(ep, h, body, fd);
        case opStatfs:
            return statfsName(ep, h, body, fd);
        case opReadlink:
            return readlinkName(ep, h, body, fd);
        case opMkdir:
        case opMknod:
        case opSymlink:
            return create(ep, h, body, fd);
        case opUnlink:
            return unlinkName(ep, h, body, fd);
        case opRename:
            return renameName(ep, h, body, fd);
        case opLink:
            return linkName(ep, h, body, fd);
        case opChmod:
        case opChown:
        case opUtimes:
        case opTruncate:
            return attr(ep, h, body, fd);
        case opFchmod:
        case opFchown:
        case opFutimes:
        case opFtruncate:
        case opFsync:
            return fattr(ep, h, body, fd);
        case opRead:
            return readNode(ep, h, body, fd);
        case opWrite:
            return writeNode(ep, h, body, fd);
        case opReaddir:
            return readdirNode(ep->node, body, fd);
        case opFstat: {
            const Stat st = statOf(*ep->server, ep->node);
            ok(fd, &st, sizeof st);
            return false;
        }
        case opFstatfs: {
            const Statfs st = statfsOf(*ep->server);
            ok(fd, &st, sizeof st);
            return false;
        }
        case opFlock:
            return flockNode(ep, h, fd);
        case opLock:
            return lockNode(ep, h, body, fd);
        case opRegister:
            return registerName(ep, h, body, fd);
        default:
            return error(fd, ENOSYS);
        }
    }

    // The other end of an endpoint closed: the file is closed.
    static void closed(Endpoint* ep) {
        const int fd = ep->fd;
        epoll_ctl(watcher, EPOLL_CTL_DEL, fd, nullptr);
        forget(ep);
        close(fd);
    }

    static void handle(Endpoint* ep) {
        if (ep->channel) {
            // Nothing comes on a device's end of its service's pair but its
            // close; what else does is dropped.
            const ssize_t got = receivePacket(ep->fd, packet, sizeof packet, packetFds, packetFdCount);
            for (usize i = 0; i < packetFdCount; ++i) {
                close(packetFds[i]);
            }
            if (got <= 0) {
                closed(ep);
            }
            return;
        }
        Header h;
        const i64 n = receive(ep->fd, h);
        if (n < 0) {
            closed(ep);
            return;
        }
        if (packetFdCount == 0) {
            return;  // a request without its reply endpoint: nobody to answer
        }
        if (h.op == opOk || h.op == opErr) {
            for (usize i = 0; i < packetFdCount; ++i) {
                close(packetFds[i]);
            }
            return;
        }
        const int fd = packetFds[packetFdCount - 1];
        if (!dispatch(ep, h, Body{packet + sizeof(Header), packet + sizeof(Header) + n}, fd)) {
            close(fd);
        }
        // The request's other descriptors, a READ's or WRITE's window, are
        // done with once it is answered; one a call kept is -1.
        for (usize i = 0; i + 1 < packetFdCount; ++i) {
            if (packetFds[i] >= 0) {
                close(packetFds[i]);
            }
        }
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
        int status;
        for (pid_t pid; (pid = waitpid(-1, &status, WNOHANG)) > 0;) {
            if (pid == shellPid) {
                shellStatus = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
            }
        }
        return shellStatus < 0;
    }

    int upEnd = -1;  // ours of the pair the services close once they are up
    Endpoint upMarker;  // what the events of upEnd carry

    static void up();

    // Serves the servers until the last endpoint of them all closes: then
    // nobody can reach them; or until the shell ends. The end of upEnd, no
    // packet on it, is the services' being up.
    static void serve() {
        epoll_event events[64];
        while (endpointCount > 0) {
            const int n = epoll_pwait(watcher, events, 64, -1, waitMasked ? &waitMask : nullptr);
            if (!reap()) {
                return;
            }
            for (int i = 0; i < n; ++i) {
                if (events[i].data.ptr == &upMarker) {
                    if (recv(upEnd, packet, sizeof packet, MSG_DONTWAIT) == 0) {
                        up();
                    }
                    continue;
                }
                handle(static_cast<Endpoint*>(events[i].data.ptr));
            }
        }
        close(watcher);
        watcher = -1;
    }

    // ---- the system ----------------------------------------------------------------

    // Mounts a server of this process on a directory of ours, a path of
    // names from our root: a walk goes into its root there, and by ".."
    // from its root back here.
    static void mountOn(Server& s, const char* path, Server& target) {
        Node* n = s.root;
        Walk walk;
        walk.push(literal(path));
        for (Name name; walk.take(name);) {
            const Entry* entry = n->kind == Kind::Dir ? lookup(n, name) : nullptr;
            if (!entry) {
                fatal("mount on %s", path);
            }
            n = entry->node;
        }
        if (n->kind != Kind::Dir) {
            fatal("mount on %s", path);
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
            fatal("rootfs: sigprocmask: errno %d", errno);
        }
        waitMasked = true;
    }

    // A field of a ustar header: its bytes up to a NUL.
    static Name field(const u8* at, usize size) {
        const char* text = reinterpret_cast<const char*>(at);
        return {text, strnlen(text, size)};
    }

    // A number field of a ustar header: octal.
    static u64 octal(const u8* at, usize size) {
        char text[16];
        const Name digits = field(at, MIN(size, sizeof text - 1));
        memcpy(text, digits.at, digits.length);
        text[digits.length] = 0;
        return strtoull(text, nullptr, 8);
    }

    // The directory of a path, made on the way.
    static Node* dirOf(Server& s, Name path) {
        Node* n = s.root;
        Walk walk;
        walk.push(path);
        for (Name part; walk.take(part);) {
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
        const usize total = rootfs_image_size;
        for (usize at = 0; at + 512 <= total && rootfs_image[at] != 0;) {
            const u8* block = rootfs_image + at;
            Name path = field(block, 100);
            const u32 perm = u32(octal(block + 100, 8)) & 0777;
            const u64 size = octal(block + 124, 12);
            const char kind = char(block[156]);
            const Name target = field(block + 157, 100);
            if (size > total - at - 512) {
                fatal("rootfs: the image ends in an entry");
            }
            while (!path.empty() && path.at[path.length - 1] == '/') {
                --path.length;
            }
            const char* slash = static_cast<const char*>(memrchr(path.at, '/', path.length));
            Node* dir = dirOf(s, {path.at, slash ? usize(slash - path.at) : 0});
            const Name base = slash ? Name{slash + 1, path.length - usize(slash + 1 - path.at)} : path;
            switch (kind) {
            case '5':
                if (!base.empty()) {
                    const Entry* entry = lookup(dir, base);
                    (entry ? entry->node : makeNode(s, dir, base, Kind::Dir, false))->perm = perm;
                }
                break;
            case '2': {
                unname(s, dir, base);
                Node* n = makeNode(s, dir, base, Kind::Link, false);
                n->content.view = reinterpret_cast<const u8*>(target.at);
                n->content.viewSize = target.length;
                break;
            }
            default: {
                unname(s, dir, base);
                Node* n = makeNode(s, dir, base, Kind::File, false);
                n->content.view = block + 512;
                n->content.viewSize = usize(size);
                n->perm = perm;
                s.used += size;
            }
            }
            at += 512 + usize((size + 511) / 512 * 512);
        }
    }

    // The terminal is descriptor 0, from the kernel: the object itself goes
    // to whoever opens /dev/tty. /dev/init takes the devices of services,
    // from the system's user.
    static void devfs(Server& s) {
        s.root = makeNode(s, nullptr, {}, Kind::Dir);
        makeNode(s, s.root, literal("tty"), Kind::Object, false)->fd = STDIN_FILENO;
        Node* init = makeNode(s, s.root, literal("init"), Kind::Registry, false);
        init->perm = 0600;
        init->uid = geteuid();
        init->gid = getegid();
    }

    // A program run in a child whose root and working directory are root,
    // an end of a pair of ours: the child asks us, who serve when it does.
    // ready, when there is one, is the child's descriptor 3.
    static pid_t spawn(const char* path, char* const* argv, char* const* env, int root, int ready = -1) {
        const pid_t pid = fork();
        if (pid == 0) {
            if (waitMasked) {
                sigprocmask(SIG_SETMASK, &waitMask, nullptr);
            }
            if (fchdir(root) != 0 || chroot(".") != 0) {
                dprintf(STDERR_FILENO, "rootfs: chroot: errno %d\n", errno);
                _exit(127);
            }
            close(root);
            if (ready >= 0 && (ready == 3 ? fcntl(3, F_SETFD, 0) : dup2(ready, 3)) < 0) {
                dprintf(STDERR_FILENO, "rootfs: descriptor 3: errno %d\n", errno);
                _exit(127);
            }
            execve(path, argv, env);
            dprintf(STDERR_FILENO, "rootfs: %s: errno %d\n", path, errno);
            _exit(127);
        }
        if (pid < 0) {
            fatal("rootfs: fork: errno %d", errno);
        }
        return pid;
    }

    // What rootfs starts runs with a clean environment, its root the far
    // end of our own pair, never ours: a server does not walk its own tree
    // by path, and could not ask itself before it serves.
    char pathVariable[] = "PATH=/bin";
    char homeVariable[] = "HOME=/";
    char termVariable[] = "TERM=vt100";
    char* const environment[] = {pathVariable, homeVariable, termVariable, nullptr};
    int rootEnd = -1;

    // Starts the services of the system, /bin/devices, with the far end of
    // a pair of ours as their descriptor 3: they close it once they are up,
    // after the services they start have it. When the last one has, the
    // shell starts (up).
    static void boot() {
        int ends[2];
        if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, ends) != 0) {
            fatal("rootfs: socketpair: errno %d", errno);
        }
        char name[] = "devices";
        char option[] = "-r";
        char three[] = "3";
        char* const argv[] = {name, option, three, nullptr};
        spawn("/bin/devices", argv, environment, rootEnd, ends[1]);
        close(ends[1]);
        upEnd = ends[0];
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.ptr = &upMarker;
        if (epoll_ctl(watcher, EPOLL_CTL_ADD, upEnd, &ev) != 0) {
            fatal("rootfs: epoll_ctl: errno %d", errno);
        }
    }

    // The services are up: the shell starts on the terminal.
    static void up() {
        epoll_ctl(watcher, EPOLL_CTL_DEL, upEnd, nullptr);
        close(upEnd);
        upEnd = -1;
        char sh[] = "sh";
        char* const argv[] = {sh, nullptr};
        shellPid = spawn("/bin/sh", argv, environment, rootEnd);
        close(rootEnd);
        rootEnd = -1;
    }
}

int main() {
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
        fatal("rootfs: socketpair: errno %d", errno);
    }
    reapOnChild();
    watcher = epoll_create1(EPOLL_CLOEXEC);
    if (watcher < 0) {
        fatal("fs: epoll_create1: errno %d", errno);
    }
    listen(image, ends[0], nullptr, true, false, nullptr, {});
    rootEnd = ends[1];
    boot();
    serve();
    return MAX(shellStatus, 0);
}
