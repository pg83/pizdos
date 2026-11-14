// ext4: an ext2, ext3 or ext4 file system on a block device as a file
// system to mount, read-only, a service of rootfs's /dev/init (fs.h),
// /dev/<name>. A file's bytes go from the device straight into the
// reader's memory: a READ whose bytes are one run of blocks on the device
// goes on to the device as it came, at the run's offset, its memfd and its
// reply's endpoint with it, and the device's service answers the reader;
// the runs of any other are copied one by one from the device into the
// memfd (copy_file_range), its holes zeros. What it reads into its own
// memory is the metadata: the superblock, the group descriptors, inodes,
// directories, block maps and extents, through a cache of blocks.
//
//     ext4 [-r fd] <device> <name>
//
// -r: a descriptor to close once the file system is there.

#include <sys/param.h>
#include <sys/stat.h>

#include "fs.h"

namespace {
    // The incompatible features it reads: filetype, extents, 64bit, mmp,
    // flex_bg, csum_seed, largedir.
    constexpr uint32_t readable = 0x0002 | 0x0040 | 0x0080 | 0x0100 | 0x0200 | 0x2000 | 0x4000;
    constexpr uint32_t extentsFlag = 0x80000;
    constexpr size_t inodeBytes = 128;

    int device = -1;
    uint32_t blockSize;
    uint64_t inodes;
    uint32_t inodesPerGroup;
    uint32_t inodeSize;
    uint32_t descSize;
    bool filetype;
    unsigned char* groups;  // the group descriptors
    const unsigned char zeros[65536] = {};

    uint16_t le16(const unsigned char* at) {
        uint16_t v;
        memcpy(&v, at, sizeof v);
        return v;
    }

    uint32_t le32(const unsigned char* at) {
        uint32_t v;
        memcpy(&v, at, sizeof v);
        return v;
    }

    // A block of the device, through a cache of 256 by their numbers: valid
    // until the next block is asked for.
    struct Cached {
        uint64_t block;
        bool valid;
        unsigned char* bytes;
    };
    Cached cache[256];

    const unsigned char* blockAt(uint64_t block) {
        Cached& c = cache[block % (sizeof cache / sizeof *cache)];
        if (c.valid && c.block == block) {
            return c.bytes;
        }
        if (!c.bytes) {
            c.bytes = static_cast<unsigned char*>(fs::allocate(blockSize));
        }
        c.valid = pread(device, c.bytes, blockSize, off_t(block * blockSize)) == ssize_t(blockSize);
        c.block = block;
        return c.valid ? c.bytes : nullptr;
    }

    // A node's inode, its first bytes, kept with it once loaded.
    const unsigned char* inodeOf(const fs::Node* n) {
        return static_cast<const unsigned char*>(n->data);
    }

    int loadNode(fs::Node* n) {
        if (n->ino == 0 || n->ino > inodes) {
            return EIO;
        }
        const uint64_t group = (n->ino - 1) / inodesPerGroup;
        const uint64_t byte = (n->ino - 1) % inodesPerGroup * inodeSize;
        const unsigned char* desc = groups + group * descSize;
        uint64_t table = le32(desc + 8);
        if (descSize >= 64) {
            table |= uint64_t(le32(desc + 40)) << 32;
        }
        const unsigned char* block = blockAt(table + byte / blockSize);
        if (!block) {
            return EIO;
        }
        auto* inode = static_cast<unsigned char*>(fs::allocate(inodeBytes));
        memcpy(inode, block + byte % blockSize, inodeBytes);
        n->data = inode;
        n->mode = le16(inode);
        n->uid = le16(inode + 2) | uint32_t(le16(inode + 120)) << 16;
        n->gid = le16(inode + 24) | uint32_t(le16(inode + 122)) << 16;
        n->size = le32(inode + 4);
        if (S_ISREG(n->mode)) {
            n->size |= uint64_t(le32(inode + 108)) << 32;
        }
        n->atime = uint64_t(le32(inode + 8)) * 1000000000;
        n->ctime = uint64_t(le32(inode + 12)) * 1000000000;
        n->mtime = uint64_t(le32(inode + 16)) * 1000000000;
        n->nlink = le16(inode + 26);
        return 0;
    }

    // The entry of an index of an extent tree's node, depth > 0, or the
    // extent of a leaf, that has block b: the device's block, 0 for none,
    // and how many blocks from b on run on, one after the other, or are
    // none.
    uint64_t mapExtents(const unsigned char* node, uint64_t b, uint64_t& run, int depth) {
        if (le16(node) != 0xF30A || depth > 5) {
            run = 1;
            return 0;
        }
        const uint32_t entries = le16(node + 2);
        if (le16(node + 6) == 0) {
            for (uint32_t i = 0; i < entries; ++i) {
                const unsigned char* e = node + 12 + 12 * i;
                const uint64_t first = le32(e);
                uint32_t length = le16(e + 4);
                const bool unwritten = length > 32768;
                if (unwritten) {
                    length -= 32768;
                }
                if (b < first) {
                    run = first - b;
                    return 0;
                }
                if (b < first + length) {
                    run = first + length - b;
                    return unwritten ? 0 : (uint64_t(le16(e + 6)) << 32 | le32(e + 8)) + (b - first);
                }
            }
            run = UINT32_MAX;
            return 0;
        }
        uint32_t i = 0;
        while (i + 1 < entries && le32(node + 12 + 12 * (i + 1)) <= b) {
            ++i;
        }
        const unsigned char* index = node + 12 + 12 * i;
        if (entries == 0 || le32(index) > b) {
            run = entries == 0 ? UINT32_MAX : le32(index) - b;
            return 0;
        }
        const uint64_t bound = i + 1 < entries ? le32(node + 12 + 12 * (i + 1)) - b : UINT64_MAX;
        const unsigned char* child = blockAt(uint64_t(le16(index + 8)) << 32 | le32(index + 4));
        if (!child) {
            run = 1;
            return 0;
        }
        const uint64_t at = mapExtents(child, b, run, depth + 1);
        run = MIN(run, bound);
        return at;
    }

    // The entry i of an indirect block, 0 for none.
    uint64_t indirect(uint64_t block, uint64_t i) {
        const unsigned char* bytes = block ? blockAt(block) : nullptr;
        return bytes ? le32(bytes + 4 * i) : 0;
    }

    // The device's block of a file's block b by the inode's map of
    // blocks, ext2's and ext3's, 0 for none.
    uint64_t mapOne(const unsigned char* inode, uint64_t b) {
        const uint64_t per = blockSize / 4;
        if (b < 12) {
            return le32(inode + 40 + 4 * b);
        }
        b -= 12;
        if (b < per) {
            return indirect(le32(inode + 88), b);
        }
        b -= per;
        if (b < per * per) {
            return indirect(indirect(le32(inode + 92), b / per), b % per);
        }
        b -= per * per;
        return indirect(indirect(indirect(le32(inode + 96), b / (per * per)), b / per % per), b % per);
    }

    // The device's block of a file's block b, 0 for none, and how many
    // blocks from b on run on, one after the other, or are none.
    uint64_t map(const unsigned char* inode, uint64_t b, uint64_t& run) {
        if ((le32(inode + 32) & extentsFlag) != 0) {
            return mapExtents(inode + 40, b, run, 0);
        }
        const uint64_t at = mapOne(inode, b);
        for (run = 1; run < 4096 && mapOne(inode, b + run) == (at ? at + run : 0); ++run) {
        }
        return at;
    }

    // ext2's types of directory entries as dirent's.
    uint8_t direntType(uint8_t type) {
        static const uint8_t types[] = {DT_UNKNOWN, DT_REG, DT_DIR, DT_CHR, DT_BLK, DT_FIFO, DT_SOCK, DT_LNK};
        return type < sizeof types ? types[type] : DT_UNKNOWN;
    }

    // A directory's entries, block by block, but "." and "..".
    int listDir(fs::Node* dir) {
        const unsigned char* inode = inodeOf(dir);
        const uint64_t blocks = (dir->size + blockSize - 1) / blockSize;
        for (uint64_t b = 0; b < blocks; ++b) {
            uint64_t run;
            const uint64_t at = map(inode, b, run);
            const unsigned char* bytes = at ? blockAt(at) : nullptr;
            if (!bytes) {
                continue;
            }
            for (uint32_t off = 0; off + 8 <= blockSize;) {
                const unsigned char* e = bytes + off;
                const uint32_t ino = le32(e);
                const uint32_t record = le16(e + 4);
                const uint32_t length = filetype ? e[6] : le16(e + 6);
                if (record < 8 || off + record > blockSize) {
                    return EIO;
                }
                const bool dots = (length == 1 && e[8] == '.') || (length == 2 && e[8] == '.' && e[9] == '.');
                if (ino != 0 && length > 0 && 8 + length <= record && !dots) {
                    fs::add(dir, reinterpret_cast<const char*>(e + 8), length, ino, filetype ? direntType(e[7]) : DT_UNKNOWN);
                }
                off += record;
            }
        }
        return 0;
    }

    // A link's target: in the inode itself when short, else in its blocks.
    int linkTarget(fs::Node* link, char* out, size_t room, size_t* length) {
        const unsigned char* inode = inodeOf(link);
        if (link->size >= room) {
            return ENAMETOOLONG;
        }
        if (link->size < 60 && (le32(inode + 32) & extentsFlag) == 0) {
            memcpy(out, inode + 40, link->size);
            *length = link->size;
            return 0;
        }
        for (uint64_t done = 0; done < link->size;) {
            uint64_t run;
            const uint64_t at = map(inode, done / blockSize, run);
            const unsigned char* bytes = at ? blockAt(at) : nullptr;
            if (!bytes) {
                return EIO;
            }
            const uint64_t n = MIN(uint64_t(blockSize), link->size - done);
            memcpy(out + done, bytes, n);
            done += n;
        }
        *length = link->size;
        return 0;
    }

    // count bytes of the file at off into the memfd at woff: handed on to
    // the device when they are one run of it, else copied run by run.
    void readFile(fs::Node* file, const service::Request& r, uint64_t off, uint32_t count, uint32_t woff) {
        const unsigned char* inode = inodeOf(file);
        uint64_t run;
        uint64_t at = map(inode, off / blockSize, run);
        const uint64_t within = off % blockSize;
        if (at != 0 && run * blockSize - within >= count) {
            unsigned char body[12];
            const uint64_t from = at * blockSize + within;
            memcpy(body, &from, 8);
            memcpy(body + 8, &woff, 4);
            Header h{opRead, count, r.h.uid, r.h.gid};
            const iovec parts[] = {{&h, sizeof h}, {body, sizeof body}};
            if (sendPacket(device, parts, 2, r.fds, r.count) < 0) {
                service::fail(r.reply(), EIO);
            }
            return;
        }
        for (uint64_t done = 0; done < count;) {
            const uint64_t place = off + done;
            at = map(inode, place / blockSize, run);
            const uint64_t n = MIN(run * blockSize - place % blockSize, count - done);
            for (uint64_t k = 0; k < n;) {
                ssize_t moved;
                if (at == 0) {
                    moved = pwrite(r.window(), zeros, size_t(MIN(sizeof zeros, n - k)), off_t(woff + done + k));
                } else {
                    off_t from = off_t(at * blockSize + place % blockSize + k);
                    off_t to = off_t(woff + done + k);
                    moved = copy_file_range(device, &from, r.window(), &to, size_t(n - k), 0);
                }
                if (moved <= 0) {
                    return service::fail(r.reply(), moved < 0 ? errno : EIO);
                }
                k += uint64_t(moved);
            }
            done += n;
        }
        service::answer(r.reply(), opOk, 0, &count, sizeof count);
    }

    // The superblock and the group descriptors, or why the file system is
    // none it reads.
    void mountable(const char* path) {
        unsigned char sb[1024];
        if (pread(device, sb, sizeof sb, 1024) != ssize_t(sizeof sb) || le16(sb + 56) != 0xEF53) {
            service::fatal("ext4: %s: no ext2 file system", path);
        }
        const uint32_t incompat = le32(sb + 96);
        if ((incompat & ~readable) != 0) {
            service::fatal("ext4: %s: features %#x unknown", path, incompat & ~readable);
        }
        const uint32_t logBlock = le32(sb + 24);
        const bool wide = (incompat & 0x80) != 0;
        blockSize = logBlock <= 6 ? 1024u << logBlock : 0;
        inodes = le32(sb);
        inodesPerGroup = le32(sb + 40);
        inodeSize = le32(sb + 76) >= 1 ? le16(sb + 88) : 128;
        descSize = wide && le16(sb + 254) >= 32 ? le16(sb + 254) : 32;
        filetype = (incompat & 0x0002) != 0;
        const uint64_t blocks = le32(sb + 4) | (wide ? uint64_t(le32(sb + 0x150)) << 32 : 0);
        const uint64_t free = le32(sb + 12) | (wide ? uint64_t(le32(sb + 0x158)) << 32 : 0);
        const uint32_t firstData = le32(sb + 20);
        const uint32_t blocksPerGroup = le32(sb + 32);
        if (blockSize == 0 || inodesPerGroup == 0 || blocksPerGroup == 0 || inodeSize < inodeBytes || blocks <= firstData) {
            service::fatal("ext4: %s: a superblock out of shape", path);
        }
        const uint64_t count = (blocks - firstData + blocksPerGroup - 1) / blocksPerGroup;
        groups = static_cast<unsigned char*>(fs::allocate(count * descSize));
        if (pread(device, groups, count * descSize, off_t((firstData + 1) * uint64_t(blockSize))) != ssize_t(count * descSize)) {
            service::fatal("ext4: %s: the group descriptors: %s", path, strerror(errno));
        }
        fs::tree.statfs = {blocks, free, inodes, le32(sb + 16), fs::device(), blockSize, NAME_MAX, 1};
    }
}

int main(int argc, char** argv) {
    int first;
    const int ready = service::ready(argc, argv, first);
    if (argc - first != 2) {
        dprintf(STDERR_FILENO, "usage: ext4 [-r fd] <device> <name>\n");
        return 2;
    }
    device = open(argv[first], O_RDONLY | O_CLOEXEC);
    if (device < 0) {
        service::fatal("ext4: %s: %s", argv[first], strerror(errno));
    }
    mountable(argv[first]);
    fs::tree.root = fs::node(2);
    fs::tree.load = loadNode;
    fs::tree.list = listDir;
    fs::tree.target = linkTarget;
    fs::tree.read = readFile;
    fs::serve("ext4", argv[first + 1], ready);
}
