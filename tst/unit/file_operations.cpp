//@ compile-flags: -g -D_GNU_SOURCE
//@ stdout: "file operations: ok\n"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <unistd.h>

int main() {
    assert(mkdir("file-case", 0700) == 0 && chdir("file-case") == 0);
    int fd = open("data", O_RDWR | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0);
    char first[] = "abc", second[] = "def";
    iovec vectors[] = {{first, 3}, {first, 0}, {second, 3}};
    assert(writev(fd, vectors, 3) == 6);
    char upper[] = "GHI";
    iovec appended[] = {{upper, 2}, {upper + 2, 1}};
    assert(pwritev(fd, appended, 2, 6) == 3);
    assert(pwrite(fd, "J", 1, 9) == 1 && lseek(fd, 0, SEEK_CUR) == 6);
    char left[4], right[6], all[16];
    iovec buffers[] = {{left, 4}, {right, 6}};
    assert(preadv(fd, buffers, 2, 0) == 10);
    assert(memcmp(left, "abcd", 4) == 0 && memcmp(right, "efGHIJ", 6) == 0);
    assert(lseek(fd, 0, SEEK_SET) == 0 && readv(fd, buffers, 2) == 10);
    assert(memcmp(left, "abcd", 4) == 0 && memcmp(right, "efGHIJ", 6) == 0);
    assert(readv(fd, nullptr, 0) == 0 && writev(fd, nullptr, 0) == 0);
    assert(readv(fd, vectors, 1025) == -1 && errno == EINVAL);
    all[0] = '!';
    assert(read(-1, all, 1) == -1 && errno == EBADF && all[0] == '!');
    assert(write(-1, all, 1) == -1 && errno == EBADF);
    assert(pread(-1, all, 1, 0) == -1 && errno == EBADF);
    assert(pwrite(-1, all, 1, 0) == -1 && errno == EBADF);
    assert(preadv(-1, buffers, 2, 0) == -1 && errno == EBADF);
    assert(pwritev(-1, buffers, 2, 0) == -1 && errno == EBADF);
    assert(pread(fd, all, 1, -1) == -1 && errno == EINVAL);
    assert(pwrite(fd, all, 1, -1) == -1 && errno == EINVAL);
    assert(fsync(fd) == 0 && fdatasync(fd) == 0 && syncfs(fd) == 0);
    assert(fsync(-1) == -1 && errno == EBADF);
    assert(fdatasync(-1) == -1 && errno == EBADF);
    assert(syncfs(-1) == -1 && errno == EBADF);
    const int advice[] = {POSIX_FADV_NORMAL, POSIX_FADV_RANDOM, POSIX_FADV_SEQUENTIAL, POSIX_FADV_WILLNEED, POSIX_FADV_DONTNEED, POSIX_FADV_NOREUSE};
    for (int hint : advice) {
        assert(posix_fadvise(fd, 0, 0, hint) == 0);
    }
    assert(posix_fadvise(-1, 0, 0, POSIX_FADV_NORMAL) == EBADF);
    assert(posix_fadvise(fd, 0, 0, 999) == EINVAL);
    assert(posix_fallocate(fd, 0, 4096) == 0);
    assert(posix_fallocate(-1, 0, 1) == EBADF);
    assert(posix_fallocate(fd, 0, 0) == EINVAL);
    struct stat metadata{};
    assert(fstat(fd, &metadata) == 0 && metadata.st_size == 4096);
    assert(ftruncate(fd, 8) == 0 && fstat(fd, &metadata) == 0 && metadata.st_size == 8);
    assert(truncate("data", 4) == 0 && fstat(fd, &metadata) == 0 && metadata.st_size == 4);
    assert(ftruncate(-1, 0) == -1 && errno == EBADF);
    assert(ftruncate(fd, -1) == -1 && errno == EINVAL);
    errno = 0;
    assert(fstat(-1, &metadata) == -1 && errno == EBADF);
    errno = 0;
    assert(lseek(-1, 0, SEEK_SET) == -1 && errno == EBADF);
    errno = 0;
    assert(isatty(-1) == 0 && errno == EBADF);
    errno = 0;
    int available;
    assert(ioctl(-1, FIONREAD, &available) == -1 && errno == EBADF);

    assert((fcntl(fd, F_GETFL) & O_ACCMODE) == O_RDWR);
    assert(fcntl(fd, F_SETFD, FD_CLOEXEC) == 0 && fcntl(fd, F_GETFD) == FD_CLOEXEC);
    assert(fcntl(fd, F_SETFD, 0) == 0 && fcntl(fd, F_GETFD) == 0);
    assert(ioctl(fd, FIOCLEX) == 0 && fcntl(fd, F_GETFD) == FD_CLOEXEC);
    assert(ioctl(fd, FIONCLEX) == 0 && fcntl(fd, F_GETFD) == 0);
    assert(fcntl(fd, F_SETFL, O_NONBLOCK | O_APPEND) == 0);
    assert((fcntl(fd, F_GETFL) & (O_NONBLOCK | O_APPEND)) == (O_NONBLOCK | O_APPEND));
    assert(fcntl(fd, F_SETFL, 0) == 0);
    assert(fcntl(fd, 99999) == -1 && errno == EINVAL);
    int duplicate = dup(fd);
    assert(duplicate >= 0 && lseek(duplicate, 2, SEEK_SET) == 2 && lseek(fd, 0, SEEK_CUR) == 2);
    assert(dup2(fd, fd) == fd && dup2(fd, 20) == 20);
    assert(dup3(fd, 21, O_CLOEXEC) == 21 && fcntl(21, F_GETFD) == FD_CLOEXEC);
    assert(dup3(fd, fd, 0) == -1 && errno == EINVAL);
    assert(dup3(fd, 21, O_NONBLOCK) == -1 && errno == EINVAL);
    assert(dup(-1) == -1 && errno == EBADF);
    assert(dup2(-1, 20) == -1 && errno == EBADF);
    int minimum = fcntl(fd, F_DUPFD, 10);
    int cloexec = fcntl(fd, F_DUPFD_CLOEXEC, 11);
    assert(minimum >= 10 && cloexec >= 11 && fcntl(cloexec, F_GETFD) == FD_CLOEXEC);
    assert(fcntl(fd, F_DUPFD, -1) == -1 && errno == EINVAL);
    assert(close(duplicate) == 0 && close(20) == 0 && close(21) == 0 && close(minimum) == 0 && close(cloexec) == 0);
    assert(close(duplicate) == -1 && errno == EBADF);
    struct flock lock{};
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    lock.l_len = 4;
    assert(fcntl(fd, F_GETLK, &lock) == 0 && lock.l_type == F_UNLCK);
    lock.l_type = F_WRLCK;
    assert(fcntl(fd, F_SETLK, &lock) == 0);
    lock.l_type = F_UNLCK;
    assert(fcntl(fd, F_SETLKW, &lock) == 0);
    lock.l_type = 999;
    assert(fcntl(fd, F_SETLK, &lock) == -1 && errno == EINVAL);
    assert(lseek(fd, 0, SEEK_SET) == 0);
    assert(lockf(fd, F_LOCK, 4) == 0 && lockf(fd, F_TEST, 4) == 0 && lockf(fd, F_ULOCK, 4) == 0);
    assert(lockf(fd, F_TLOCK, 4) == 0 && lockf(fd, F_ULOCK, 4) == 0);
    assert(lockf(fd, 999, 4) == -1 && errno == EINVAL);
    assert(lockf(-1, F_LOCK, 4) == -1 && errno == EBADF);
    assert(flock(fd, LOCK_EX | LOCK_NB) == 0 && flock(fd, LOCK_UN) == 0);
    assert(flock(fd, 0) == -1 && errno == EINVAL);
    assert(flock(-1, LOCK_EX) == -1 && errno == EBADF);
    assert(fchmod(fd, 0640) == 0 && fstat(fd, &metadata) == 0 && (metadata.st_mode & 0777) == 0640);
    assert(fchmod(-1, 0600) == -1 && errno == EBADF);
    assert(fchown(fd, uid_t(-1), gid_t(-1)) == 0);
    assert(fchown(-1, getuid(), getgid()) == -1 && errno == EBADF);
    timeval times[] = {{1000000000, 123456}, {1000000001, 234567}};
    assert(futimes(fd, times) == 0 && fstat(fd, &metadata) == 0);
    assert(metadata.st_atim.tv_sec == 1000000000 && metadata.st_atim.tv_nsec == 123456000);
    assert(metadata.st_mtim.tv_sec == 1000000001 && metadata.st_mtim.tv_nsec == 234567000);
    timespec nanoseconds[] = {{1000000002, 345678901}, {1000000003, 456789012}};
    assert(futimens(fd, nanoseconds) == 0 && fstat(fd, &metadata) == 0);
    assert(metadata.st_atim.tv_nsec == 345678901 && metadata.st_mtim.tv_nsec == 456789012);
    assert(futimens(fd, nullptr) == 0 && futimes(fd, nullptr) == 0);
    assert(futimes(-1, times) == -1 && errno == EBADF);
    assert(futimens(-1, nanoseconds) == -1 && errno == EBADF);
    struct statfs filesystem{};
    struct statvfs volume{};
    assert(fstatfs(fd, &filesystem) == 0 && filesystem.f_bsize > 0);
    assert(statfs(".", &filesystem) == 0 && filesystem.f_namelen > 0);
    assert(fstatvfs(fd, &volume) == 0 && volume.f_bsize > 0 && volume.f_frsize > 0);
    assert(statvfs(".", &volume) == 0 && volume.f_namemax > 0);
    assert(fstatfs(-1, &filesystem) == -1 && errno == EBADF);
    assert(fstatvfs(-1, &volume) == -1 && errno == EBADF);
    assert(statfs("missing", &filesystem) == -1 && errno == ENOENT);
    assert(statvfs("missing", &volume) == -1 && errno == ENOENT);
    int target = open("copy", O_RDWR | O_CREAT | O_EXCL, 0600);
    assert(target >= 0);
    off_t from = 1, to = 0;
    assert(copy_file_range(fd, &from, target, &to, 2, 0) == 2 && from == 3 && to == 2);
    assert(pread(target, all, 2, 0) == 2 && memcmp(all, "bc", 2) == 0);
    assert(copy_file_range(fd, nullptr, target, nullptr, 2, 0) == 2);
    assert(pread(target, all, 2, 0) == 2 && memcmp(all, "ab", 2) == 0);
    assert(copy_file_range(-1, nullptr, target, nullptr, 1, 0) == -1 && errno == EBADF);
    assert(copy_file_range(fd, nullptr, -1, nullptr, 1, 0) == -1 && errno == EBADF);
    assert(copy_file_range(fd, nullptr, target, nullptr, 1, 1) == -1 && errno == EINVAL);
    assert(close(target) == 0 && close(fd) == 0);
    puts("file operations: ok");
}
