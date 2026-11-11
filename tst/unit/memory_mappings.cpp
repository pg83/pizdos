//@ compile-flags: -g -D_GNU_SOURCE -fno-builtin
//@ stdout: "memory mappings: ok\n"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

int main() {
    constexpr size_t page = 4096;
    assert(sysconf(_SC_PAGESIZE) == page);
    assert(mmap(nullptr, 0, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED && errno == EINVAL);
    assert(munmap(nullptr, 0) == -1 && errno == EINVAL);
    assert(mmap(nullptr, page, PROT_READ, MAP_PRIVATE, -1, 0) == MAP_FAILED && errno == EBADF);
    int directory = open(".", O_RDONLY | O_DIRECTORY);
    assert(directory >= 0);
    assert(mmap(nullptr, page, PROT_READ, MAP_PRIVATE, directory, 0) == MAP_FAILED);
    assert(close(directory) == 0);
    auto* anonymous = static_cast<unsigned char*>(mmap(nullptr, page * 2, PROT_READ | PROT_WRITE,
                                                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    assert(anonymous != MAP_FAILED && uintptr_t(anonymous) % page == 0);
    for (size_t i = 0; i < page * 2; ++i) {
        assert(anonymous[i] == 0);
        anonymous[i] = 0x57;
    }
    assert(madvise(anonymous, page * 2, MADV_NORMAL) == 0);
    assert(madvise(anonymous, page * 2, MADV_RANDOM) == 0);
    assert(madvise(anonymous, page * 2, MADV_SEQUENTIAL) == 0);
    assert(madvise(anonymous, page * 2, MADV_WILLNEED) == 0);
    assert(anonymous[page] == 0x57);
    assert(madvise(anonymous, page, MADV_DONTNEED) == 0);
    assert(anonymous[0] == 0 && anonymous[page - 1] == 0 && anonymous[page] == 0x57);
    assert(mprotect(anonymous, page * 2, PROT_READ) == 0 && anonymous[page] == 0x57);
    assert(mprotect(anonymous, page * 2, PROT_READ | PROT_WRITE) == 0);
    assert(msync(anonymous, page * 2, MS_SYNC) == 0);
    assert(munmap(anonymous, page * 2) == 0);
    void* small[3];
    for (auto& mapping : small) {
        mapping = mmap(nullptr, 1, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        assert(mapping != MAP_FAILED && uintptr_t(mapping) % page == 0);
        assert(*static_cast<unsigned char*>(mapping) == 0);
        *static_cast<unsigned char*>(mapping) = 1;
    }
    for (auto mapping : small) {
        assert(munmap(mapping, 1) == 0);
    }

    int fd = open("mapping.data", O_RDWR | O_CREAT | O_TRUNC, 0600);
    assert(fd >= 0);
    unsigned char block[page];
    for (unsigned i = 0; i < 3; ++i) {
        memset(block, 'a' + i, sizeof(block));
        assert(write(fd, block, sizeof(block)) == sizeof(block));
    }
    auto* shared = static_cast<unsigned char*>(mmap(nullptr, page * 2, PROT_READ | PROT_WRITE, MAP_SHARED, fd, page));
    assert(shared != MAP_FAILED && shared[0] == 'b' && shared[page] == 'c');
    shared[0] = 'x';
    shared[1] = 'y';
    shared[page + 7] = 'z';
    assert(msync(shared, page, MS_SYNC) == 0);
    assert(pread(fd, block, 2, page) == 2 && block[0] == 'x' && block[1] == 'y');
    assert(msync(shared + page, page, MS_ASYNC) == 0);
    assert(msync(shared, page * 2, MS_SYNC) == 0);
    assert(pread(fd, block, 1, page * 2 + 7) == 1 && block[0] == 'z');
    assert(madvise(shared, page * 2, MADV_DONTNEED) == 0);
    assert(shared[0] == 'x' && shared[page + 7] == 'z');
    assert(close(fd) == 0);
    shared[3] = 'q';
    assert(munmap(shared, page * 2) == 0);
    fd = open("mapping.data", O_RDWR);
    assert(fd >= 0 && pread(fd, block, 1, page + 3) == 1 && block[0] == 'q');

    auto* privateCopy = static_cast<unsigned char*>(mmap(nullptr, page, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0));
    assert(privateCopy != MAP_FAILED && privateCopy[0] == 'a');
    privateCopy[0] = 'p';
    assert(msync(privateCopy, page, MS_SYNC) == 0);
    assert(pread(fd, block, 1, 0) == 1 && block[0] == 'a');
    assert(madvise(privateCopy, page, MADV_DONTNEED) == 0 && privateCopy[0] == 'a');
    assert(munmap(privateCopy, page) == 0);
    assert(mmap(nullptr, page, PROT_READ, MAP_PRIVATE, fd, 1) == MAP_FAILED && errno == EINVAL);
    assert(mmap(nullptr, page, PROT_READ, 0, fd, 0) == MAP_FAILED && errno == EINVAL);
    shared = static_cast<unsigned char*>(mmap(nullptr, page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    assert(shared != MAP_FAILED);
    assert(msync(shared, page, MS_SYNC | MS_ASYNC) == -1 && errno == EINVAL);
    assert(msync(shared, page, 0x80) == -1 && errno == EINVAL);
    assert(msync(shared + 1, page - 1, MS_SYNC) == -1 && errno == EINVAL);
    assert(munmap(shared, page) == 0);
    assert(msync(reinterpret_cast<void*>(page), page, MS_SYNC) == -1 && errno == ENOMEM);
    assert(close(fd) == 0 && unlink("mapping.data") == 0);
#ifdef __wasm__
    // The VM's memory needs no page locking and keeps its heap allocation.
    assert(mlock(block, sizeof(block)) == 0 && munlock(block, sizeof(block)) == 0);
    assert(mlockall(MCL_CURRENT | MCL_FUTURE) == 0 && munlockall() == 0);
    assert(malloc_trim(0) == 0);
#endif
    puts("memory mappings: ok");
}
