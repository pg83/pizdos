// A file system's server, read-only, for a service of the system
// (service.h, protocol.h): the walks, names and calls of the protocol on
// a tree of nodes the service gives (Tree), and its mounts. A node is made
// when a listing of its directory names it, its attributes loaded when
// they are asked for; a file's bytes are the service's to put into the
// reader's memory or to hand the READ on to its device.
//
// Every endpoint here is an open directory or file: an OPEN rootfs hands
// on from the service's name in /dev, an OPEN of a directory here, or a
// walk's reply. A walk always ends kept, its reply's endpoint the
// directory of its last name: the kernel may have asked another server,
// which handed the walk on. A walk out of the root by ".." goes on to
// rootfs, "../" and the names left, through the endpoint of the mount
// point the walk came in by (MOUNTED), and so does the path of a
// directory, which starts with the mount point's. Without a mount the
// root is its own parent.

#pragma once

#include <dirent.h>
#include <sys/param.h>
#include <sys/stat.h>

#include "service.h"

namespace fs {
    struct Node;

    struct Entry {
        Node* node;
        uint8_t type;  // DT_*
        uint32_t length;
        char* name;
    };

    struct Node {
        uint64_t ino;
        bool loaded;  // the attributes are there
        uint32_t mode;
        uint32_t nlink;
        uint32_t uid;
        uint32_t gid;
        uint64_t size;
        uint64_t atime;  // nanoseconds
        uint64_t mtime;
        uint64_t ctime;
        Node* parent;  // a directory's, by the name it was found by; the root's is itself
        char* name;
        uint32_t nameLength;
        bool listed;  // a directory's entries are there
        Entry* entries;
        size_t count;
        size_t room;
        Node* next;  // in its bucket
        void* data;  // the service's
    };

    // What a service gives: its root and its statfs, and how to load a
    // node's attributes, list a directory (add), read a link's target and
    // answer a READ: count bytes at off, within the file, into the memfd
    // of the request at woff, answered on r.reply() or handed on with the
    // request's descriptors. 0, or the errno.
    struct Tree {
        Node* root;
        Statfs statfs;
        int (*load)(Node* n);
        int (*list)(Node* dir);
        int (*target)(Node* link, char* out, size_t room, size_t* length);
        void (*read)(Node* file, const service::Request& r, uint64_t off, uint32_t count, uint32_t woff);
    };

    // A mount the service is under: the endpoint of the mount point rootfs
    // gave it, while an endpoint that came in by it is open.
    struct Mount {
        int up;
        uint32_t refs;
    };

    // An endpoint's: the node it is open on, the mount it came in by, the
    // directory and name a file was opened by.
    struct Open {
        Node* node;
        Mount* mount;
        Node* dir;
        char name[NAME_MAX + 1];
    };

    inline Tree tree;
    inline int epoll = -1;
    inline Node* buckets[4096];

    inline void* allocate(size_t size) {
        void* p = calloc(1, size);
        if (!p) {
            service::fatal("fs: out of memory");
        }
        return p;
    }

    // The node of an inode, made when there is none.
    inline Node* node(uint64_t ino) {
        Node*& bucket = buckets[ino % (sizeof buckets / sizeof *buckets)];
        for (Node* n = bucket; n; n = n->next) {
            if (n->ino == ino) {
                return n;
            }
        }
        auto* n = static_cast<Node*>(allocate(sizeof(Node)));
        n->ino = ino;
        n->next = bucket;
        bucket = n;
        return n;
    }

    // A name of a listing of dir: the entry, and its node.
    inline void add(Node* dir, const char* name, uint32_t length, uint64_t ino, uint8_t type) {
        if (dir->count == dir->room) {
            dir->room = dir->room ? dir->room * 2 : 16;
            dir->entries = static_cast<Entry*>(realloc(dir->entries, dir->room * sizeof(Entry)));
            if (!dir->entries) {
                service::fatal("fs: out of memory");
            }
        }
        auto* bytes = static_cast<char*>(allocate(length + 1));
        memcpy(bytes, name, length);
        dir->entries[dir->count++] = {node(ino), type, length, bytes};
    }

    inline int loaded(Node* n) {
        if (!n->loaded) {
            if (const int e = tree.load(n)) {
                return e;
            }
            n->loaded = true;
        }
        return 0;
    }

    inline int listed(Node* dir) {
        if (!dir->listed) {
            if (const int e = tree.list(dir)) {
                return e;
            }
            dir->listed = true;
        }
        return 0;
    }

    inline bool isDir(const Node* n) {
        return S_ISDIR(n->mode);
    }

    // A name of a directory: its node, found by that name, or 0 and errno.
    inline Node* child(Node* dir, const char* name, size_t length, int& e) {
        if ((e = listed(dir)) != 0) {
            return nullptr;
        }
        for (size_t i = 0; i < dir->count; ++i) {
            const Entry& entry = dir->entries[i];
            if (entry.length == length && memcmp(entry.name, name, length) == 0) {
                Node* n = entry.node;
                if ((e = loaded(n)) != 0) {
                    return nullptr;
                }
                if (isDir(n) && !n->parent) {
                    n->parent = dir;
                    n->name = entry.name;
                    n->nameLength = entry.length;
                }
                return n;
            }
        }
        e = ENOENT;
        return nullptr;
    }

    inline Stat statOf(const Node* n) {
        Stat st{};
        st.ino = n->ino;
        st.size = n->size;
        st.mtime = n->mtime;
        st.mode = n->mode;
        st.nlink = n->nlink;
        st.uid = n->uid;
        st.gid = n->gid;
        st.atime = n->atime;
        st.dev = tree.statfs.dev;
        st.ctime = n->ctime;
        return st;
    }

    // Whether who asks may read (4) or search (1) a node: root everything
    // but executing what nobody may, else the bits of who they are to it.
    inline bool may(const Node* n, const Header& h, uint32_t want) {
        if (h.uid == 0) {
            return (want & 1) == 0 || isDir(n) || (n->mode & 0111) != 0;
        }
        const uint32_t bits = h.uid == n->uid ? n->mode >> 6 : h.gid == n->gid ? n->mode >> 3 : n->mode;
        return (bits & want) == want;
    }

    // An endpoint of a node: watched, its descriptor the open's now.
    inline void keep(service::Request& r, Node* n, Mount* mount, Node* dir, const char* name, size_t length, bool reads) {
        auto* o = static_cast<Open*>(allocate(sizeof(Open)));
        o->node = n;
        o->mount = mount;
        o->dir = dir;
        memcpy(o->name, name, MIN(length, size_t(NAME_MAX)));
        if (mount) {
            ++mount->refs;
        }
        service::watch(epoll, {r.reply(), false, reads, false, o});
        r.fds[r.count - 1] = -1;
    }

    inline void gone(service::Endpoint& e) {
        auto* o = static_cast<Open*>(e.of);
        if (o && o->mount && --o->mount->refs == 0) {
            close(o->mount->up);
            free(o->mount);
        }
        free(o);
    }

    // The names of a walk: the path's, and before them the targets of the
    // links it went through, the last on top.
    struct Walk {
        struct Part {
            const char* at;
            size_t length;
            char* own;  // a link's target
        };
        Part stack[41];
        int depth = 0;

        ~Walk() {
            for (int i = 0; i < depth; ++i) {
                free(stack[i].own);
            }
        }

        void push(const char* at, size_t length, char* own = nullptr) {
            stack[depth++] = {at, length, own};
        }

        // The next name, or false at the end.
        bool take(const char*& name, size_t& length) {
            for (; depth > 0; --depth) {
                Part& top = stack[depth - 1];
                while (top.length && *top.at == '/') {
                    ++top.at;
                    --top.length;
                }
                if (top.length) {
                    const auto* slash = static_cast<const char*>(memchr(top.at, '/', top.length));
                    name = top.at;
                    length = slash ? size_t(slash - top.at) : top.length;
                    top.at += length;
                    top.length -= length;
                    return true;
                }
                free(top.own);
                top.own = nullptr;
            }
            return false;
        }

        bool more() const {
            for (int i = 0; i < depth; ++i) {
                for (size_t k = 0; k < stack[i].length; ++k) {
                    if (stack[i].at[k] != '/') {
                        return true;
                    }
                }
            }
            return false;
        }

        // What is left, joined by "/", after first: the names a walk handed
        // on goes on with. False when it does not fit.
        bool rest(const char* first, char* out, size_t room, size_t& length) {
            length = 0;
            auto put = [&](const char* at, size_t n) {
                if (n > room - length) {
                    return false;
                }
                memcpy(out + length, at, n);
                length += n;
                return true;
            };
            if (!put(first, strlen(first))) {
                return false;
            }
            const char* name;
            size_t n;
            while (take(name, n)) {
                if (!put("/", 1) || !put(name, n)) {
                    return false;
                }
            }
            return true;
        }
    };

    inline bool isRoot(const Node* n, uint64_t dev, uint64_t ino) {
        return tree.statfs.dev == dev && n->ino == ino;
    }

    // A walk handed on through a mount's endpoint, as RESOLVE: the kernel's
    // reply endpoint goes with it, rootfs answers.
    inline void handUp(const service::Request& r, const Mount* mount, uint64_t dev, uint64_t ino, uint32_t links, const char* path, size_t length) {
        unsigned char body[20];
        memcpy(body, &dev, 8);
        memcpy(body + 8, &ino, 8);
        memcpy(body + 16, &links, 4);
        Header h{opResolve, resolveHanded, r.h.uid, r.h.gid};
        const iovec parts[] = {{&h, sizeof h}, {body, sizeof body}, {const_cast<char*>(path), length}};
        if (sendPacket(mount->up, parts, 3, r.fds, r.count) < 0) {
            service::fail(r.reply(), EIO);
        }
    }

    // A walk from an endpoint's directory to the directory of its last
    // name, as rootfs walks one (rootfs.cpp resolve).
    inline void resolve(Open& o, service::Request& r) {
        uint64_t dev;
        uint64_t ino;
        uint32_t links;
        if (!r.at(0, dev) || !r.at(8, ino) || !r.at(16, links)) {
            return service::fail(r.reply(), EINVAL);
        }
        const char* path = reinterpret_cast<const char*>(r.body + 20);
        const size_t size = r.size - 20;
        const bool slash = size > 0 && path[size - 1] == '/';
        Walk walk;
        walk.push(path, size);
        Node* here = o.node;
        const char* name = nullptr;
        size_t length = 0;
        bool have = walk.take(name, length);
        static char left[PATH_MAX + 4];
        size_t leftLength;
        for (; have && walk.more(); have = walk.take(name, length)) {
            if (!isDir(here)) {
                return service::fail(r.reply(), ENOTDIR);
            }
            if (!may(here, r.h, 1)) {
                return service::fail(r.reply(), EACCES);
            }
            if (length == 1 && name[0] == '.') {
                continue;
            }
            if (length == 2 && name[0] == '.' && name[1] == '.') {
                if (here == tree.root && !isRoot(here, dev, ino) && o.mount) {
                    if (!walk.rest("..", left, sizeof left, leftLength)) {
                        return service::fail(r.reply(), ENAMETOOLONG);
                    }
                    return handUp(r, o.mount, dev, ino, links, left, leftLength);
                }
                if (!isRoot(here, dev, ino)) {
                    here = here->parent;
                }
                continue;
            }
            int e;
            Node* next = child(here, name, length, e);
            if (!next) {
                return service::fail(r.reply(), e);
            }
            if (!S_ISLNK(next->mode)) {
                here = next;
                continue;
            }
            if (links >= 40) {
                return service::fail(r.reply(), ELOOP);
            }
            auto* target = static_cast<char*>(allocate(PATH_MAX));
            size_t n = 0;
            if ((e = tree.target(next, target, PATH_MAX, &n)) != 0 || n == 0) {
                free(target);
                return service::fail(r.reply(), e ? e : ENOENT);
            }
            ++links;
            walk.push(target, n, target);
            if (target[0] == '/') {
                // An absolute link: the kernel goes on from the root of the
                // process with the names left.
                if (!walk.rest("", left, sizeof left, leftLength)) {
                    return service::fail(r.reply(), ENAMETOOLONG);
                }
                if (slash && leftLength < sizeof left) {
                    left[leftLength++] = '/';
                }
                unsigned char head[16];
                const uint32_t fields[4] = {stepRoot, links, lastNone, 0};
                memcpy(head, fields, sizeof head);
                static unsigned char out[sizeof head + sizeof left];
                memcpy(out, head, sizeof head);
                // rest() began with "" and a "/": the names, from the root.
                const size_t skip = leftLength > 0 && left[0] == '/' ? 1 : 0;
                memcpy(out + sizeof head, left + skip, leftLength - skip);
                return service::answer(r.reply(), opOk, 0, out, sizeof head + leftLength - skip);
            }
        }
        if (!isDir(here)) {
            return service::fail(r.reply(), ENOTDIR);
        }
        uint32_t last = lastNone;
        if (have) {
            if (!may(here, r.h, 1)) {
                return service::fail(r.reply(), EACCES);
            }
            if (length == 1 && name[0] == '.') {
                last = lastDot;
            } else if (length == 2 && name[0] == '.' && name[1] == '.') {
                if (here == tree.root && !isRoot(here, dev, ino) && o.mount) {
                    return handUp(r, o.mount, dev, ino, links, "..", 2);
                }
                last = lastDotdot;
                if (!isRoot(here, dev, ino)) {
                    here = here->parent;
                }
            } else if (length > NAME_MAX) {
                return service::fail(r.reply(), ENAMETOOLONG);
            } else {
                last = lastName;
            }
        }
        static unsigned char out[16 + NAME_MAX];
        const uint32_t fields[4] = {stepDone, links, last, (slash && last == lastName ? stepSlash : 0) | stepKept};
        memcpy(out, fields, sizeof fields);
        const size_t named = last == lastName ? length : 0;
        memcpy(out + sizeof fields, name, named);
        service::answer(r.reply(), opOk, 0, out, sizeof fields + named);
        keep(r, here, o.mount, nullptr, "", 0, false);
    }

    // What a call on a name of a directory names: the directory itself for
    // "", else its node; 0 and errno when there is none.
    inline Node* named(Open& o, const service::Request& r, size_t at, const char*& name, size_t& length, int& e) {
        name = reinterpret_cast<const char*>(r.body + at);
        length = r.size > at ? strnlen(name, r.size - at) : 0;
        if (length == 0) {
            e = 0;
            return o.node;
        }
        if (!isDir(o.node)) {
            e = ENOTDIR;
            return nullptr;
        }
        if (!may(o.node, r.h, 1)) {
            e = EACCES;
            return nullptr;
        }
        return child(o.node, name, length, e);
    }

    // An open of a name of the directory, or of the directory itself: read
    // only, a directory or a file; a link is ELOOP for the kernel to follow
    // but with O_PATH and O_NOFOLLOW; what is no file nor directory has no
    // driver here.
    inline void open(Open& o, service::Request& r) {
        const char* name;
        size_t length;
        int e;
        const uint32_t flags = r.h.arg;
        const bool opath = (flags & O_PATH) != 0;
        Node* n = named(o, r, 4, name, length, e);
        if (!n) {
            return service::fail(r.reply(), e == ENOENT && (flags & O_CREAT) != 0 ? EROFS : e);
        }
        if ((flags & O_CREAT) != 0 && (flags & O_EXCL) != 0) {
            return service::fail(r.reply(), EEXIST);
        }
        if (S_ISLNK(n->mode) && !(opath && (flags & O_NOFOLLOW) != 0)) {
            return service::fail(r.reply(), ELOOP);
        }
        if ((flags & O_DIRECTORY) != 0 && !isDir(n)) {
            return service::fail(r.reply(), ENOTDIR);
        }
        const bool writes = !opath && ((flags & O_ACCMODE) != O_RDONLY || (flags & O_TRUNC) != 0);
        if (writes) {
            return service::fail(r.reply(), isDir(n) ? EISDIR : EROFS);
        }
        if (!opath && !may(n, r.h, 4)) {
            return service::fail(r.reply(), EACCES);
        }
        if (!opath && (S_ISCHR(n->mode) || S_ISBLK(n->mode) || S_ISSOCK(n->mode))) {
            return service::fail(r.reply(), ENXIO);
        }
        const Stat st = statOf(n);
        service::answer(r.reply(), opOk, 0, &st, sizeof st);
        keep(r, n, o.mount, length ? o.node : nullptr, name, length, !opath);
    }

    // The path of the endpoint's directory, or its file's by the name it was
    // opened by, from the root of the process: up to it, or to this root and
    // then the mount point's path, which rootfs gives.
    inline void path(Open& o, service::Request& r) {
        uint64_t dev;
        uint64_t ino;
        if (!r.at(0, dev) || !r.at(8, ino)) {
            return service::fail(r.reply(), EINVAL);
        }
        static char out[PATH_MAX + 1];
        size_t start = sizeof out;
        auto prepend = [&start](const char* name, size_t length) {
            if (start < length + 1) {
                return false;
            }
            start -= length;
            memcpy(out + start, name, length);
            out[--start] = '/';
            return true;
        };
        Node* here = o.node;
        if (!isDir(here)) {
            if (!o.dir || !prepend(o.name, strlen(o.name))) {
                return service::fail(r.reply(), ENOENT);
            }
            here = o.dir;
        }
        while (!isRoot(here, dev, ino) && here != tree.root) {
            if (!here->name || !prepend(here->name, here->nameLength)) {
                return service::fail(r.reply(), ENOENT);
            }
            here = here->parent;
        }
        if (here == tree.root && !isRoot(here, dev, ino)) {
            // The mount point's path, asked of rootfs.
            if (!o.mount) {
                return service::fail(r.reply(), ENOENT);
            }
            int pair[2];
            if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair) != 0) {
                return service::fail(r.reply(), errno);
            }
            Header h{opPath, 0, r.h.uid, r.h.gid};
            const iovec parts[] = {{&h, sizeof h}, {const_cast<unsigned char*>(r.body), 16}};
            const bool sent = sendPacket(o.mount->up, parts, 2, &pair[1], 1) >= 0;
            close(pair[1]);
            static char got[sizeof(Header) + PATH_MAX];
            const ssize_t n = sent ? recv(pair[0], got, sizeof got, 0) : -1;
            close(pair[0]);
            Header reply;
            if (n < ssize_t(sizeof reply)) {
                return service::fail(r.reply(), EIO);
            }
            memcpy(&reply, got, sizeof reply);
            if (reply.op != opOk) {
                return service::fail(r.reply(), int(reply.arg));
            }
            size_t length = size_t(n) - sizeof reply;
            if (length == 1 && got[sizeof reply] == '/') {
                length = 0;
            }
            if (start < length) {
                return service::fail(r.reply(), ENAMETOOLONG);
            }
            start -= length;
            memcpy(out + start, got + sizeof reply, length);
        }
        if (start == sizeof out) {
            return service::answer(r.reply(), opOk, 0, "/", 1);
        }
        service::answer(r.reply(), opOk, 0, out + start, sizeof out - start);
    }

    // The entries of the directory after a position, the index of one plus
    // one, as many as one reply holds.
    inline void readdir(Open& o, const service::Request& r) {
        uint64_t after;
        if (!isDir(o.node)) {
            return service::fail(r.reply(), ENOTDIR);
        }
        if (!r.at(0, after)) {
            return service::fail(r.reply(), EINVAL);
        }
        if (const int e = listed(o.node)) {
            return service::fail(r.reply(), e);
        }
        static unsigned char out[65536 - sizeof(Header)];
        size_t size = 0;
        for (uint64_t i = after; i < o.node->count; ++i) {
            const Entry& entry = o.node->entries[i];
            if (size + 17 + entry.length + 1 > sizeof out) {
                break;
            }
            const uint64_t pos = i + 1;
            memcpy(out + size, &entry.node->ino, 8);
            memcpy(out + size + 8, &pos, 8);
            out[size + 16] = entry.type;
            memcpy(out + size + 17, entry.name, entry.length);
            out[size + 17 + entry.length] = 0;
            size += 17 + entry.length + 1;
        }
        service::answer(r.reply(), opOk, 0, out, size);
    }

    inline void handleOpen(Open& o, service::Request& r);

    // A request on an endpoint: an OPEN rootfs handed on from the service's
    // name opens the root; MOUNTED makes the endpoint a mount's.
    inline bool handle(service::Endpoint& e, service::Request& r) {
        if (e.channel) {
            Open root{tree.root, nullptr, nullptr, {}};
            const uint32_t flags = r.h.arg;
            if ((flags & O_ACCMODE) != O_RDONLY && (flags & O_PATH) == 0) {
                service::fail(r.reply(), EISDIR);
                return false;
            }
            if (!may(tree.root, r.h, (flags & O_PATH) ? 0 : 4)) {
                service::fail(r.reply(), EACCES);
                return false;
            }
            const Stat st = statOf(tree.root);
            service::answer(r.reply(), opOk, 0, &st, sizeof st);
            keep(r, root.node, nullptr, nullptr, "", 0, true);
            return false;
        }
        handleOpen(*static_cast<Open*>(e.of), r);
        return false;
    }

    inline void handleOpen(Open& o, service::Request& r) {
        switch (r.h.op) {
        case opMounted:
            if (!o.mount && r.count == 1) {
                o.mount = static_cast<Mount*>(allocate(sizeof(Mount)));
                *o.mount = {r.fds[0], 1};
                r.fds[0] = -1;
            }
            break;
        case opResolve:
            resolve(o, r);
            break;
        case opOpen:
            open(o, r);
            break;
        case opStat:
        case opReadlink:
        case opStatfs: {
            const char* name;
            size_t length;
            int e;
            Node* n = named(o, r, 0, name, length, e);
            if (!n) {
                service::fail(r.reply(), e);
            } else if (r.h.op == opStat) {
                const Stat st = statOf(n);
                service::answer(r.reply(), opOk, 0, &st, sizeof st);
            } else if (r.h.op == opStatfs) {
                service::answer(r.reply(), opOk, 0, &tree.statfs, sizeof tree.statfs);
            } else if (!S_ISLNK(n->mode)) {
                service::fail(r.reply(), EINVAL);
            } else {
                static char target[PATH_MAX];
                size_t got = 0;
                if (const int t = tree.target(n, target, sizeof target, &got)) {
                    service::fail(r.reply(), t);
                } else {
                    service::answer(r.reply(), opOk, 0, target, got);
                }
            }
            break;
        }
        case opRead: {
            uint64_t off;
            uint32_t woff;
            if (isDir(o.node)) {
                service::fail(r.reply(), EISDIR);
            } else if (!S_ISREG(o.node->mode)) {
                service::fail(r.reply(), EINVAL);
            } else if (r.window() < 0 || !r.at(0, off) || !r.at(8, woff)) {
                service::fail(r.reply(), EINVAL);
            } else if (off >= o.node->size || r.h.arg == 0) {
                const uint32_t none = 0;
                service::answer(r.reply(), opOk, 0, &none, sizeof none);
            } else {
                tree.read(o.node, r, off, uint32_t(MIN(uint64_t(r.h.arg), o.node->size - off)), woff);
            }
            break;
        }
        case opReaddir:
            readdir(o, r);
            break;
        case opFstat: {
            const Stat st = statOf(o.node);
            service::answer(r.reply(), opOk, 0, &st, sizeof st);
            break;
        }
        case opFstatfs:
            service::answer(r.reply(), opOk, 0, &tree.statfs, sizeof tree.statfs);
            break;
        case opPath:
            path(o, r);
            break;
        case opFsync:
        case opFlock:
            service::answer(r.reply(), opOk, 0);
            break;
        case opLock: {
            // No lock is ever in the way of another here.
            uint32_t type;
            uint64_t start;
            uint64_t length;
            if (r.h.arg == 1 && r.at(0, type) && r.at(4, start) && r.at(12, length)) {
                unsigned char none[20];
                const uint32_t unlocked = F_UNLCK;
                memcpy(none, &unlocked, 4);
                memcpy(none + 4, &start, 8);
                memcpy(none + 12, &length, 8);
                service::answer(r.reply(), opOk, 0, none, sizeof none);
            } else {
                service::answer(r.reply(), opOk, 0);
            }
            break;
        }
        case opWrite:
            service::fail(r.reply(), EBADF);
            break;
        default:
            service::fail(r.reply(), EROFS);
        }
    }

    // Registers the file system at /dev/init as a service to mount, and
    // serves it.
    [[noreturn]] inline void serve(const char* program, const char* name, int ready) {
        epoll = epoll_create1(EPOLL_CLOEXEC);
        if (epoll < 0) {
            service::fatal("%s: epoll_create1: errno %d", program, errno);
        }
        tree.root->parent = tree.root;
        if (const int e = loaded(tree.root)) {
            service::fatal("%s: the root: %s", program, strerror(e));
        }
        const service::Device d = service::enroll(program, name, S_IFSOCK | 0660);
        service::watch(epoll, {d.end, true, false, false, nullptr});
        if (ready >= 0) {
            close(ready);
        }
        service::serve(epoll, handle, gone);
    }

    // A device for the file system's files: the number of a pair, which no
    // other pair in the kernel has, as rootfs's are.
    inline uint64_t device() {
        int ends[2];
        struct stat st;
        if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, ends) != 0 || fstat(ends[0], &st) != 0) {
            service::fatal("fs: a pair for a device: errno %d", errno);
        }
        close(ends[0]);
        close(ends[1]);
        return uint64_t(st.st_ino);
    }
}
