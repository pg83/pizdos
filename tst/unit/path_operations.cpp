//@ compile-flags: -g -D_GNU_SOURCE
//@ stdout: "path operations: ok\n"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <utime.h>

static int reject(const dirent*) {
    return 0;
}

int main() {
    mode_t previous = umask(0);
    char temporary[] = "paths-XXXXXX";
    assert(mkdtemp(temporary) == temporary);
    struct stat metadata{};
    assert(stat(temporary, &metadata) == 0 && S_ISDIR(metadata.st_mode) && (metadata.st_mode & 0777) == 0700);
    assert(chdir(temporary) == 0);
    assert(mkdir("rename-directory", 0700) == 0);
    assert(rename("rename-directory////", "moved-directory") == 0);
    assert(stat("moved-directory", &metadata) == 0 && S_ISDIR(metadata.st_mode));
    assert(rmdir("moved-directory") == 0);
    assert(rename("", "missing") == -1 && errno == ENOENT);
    assert(rename("missing", "") == -1 && errno == ENOENT);
    assert(link("", "bad-link") == -1 && errno == ENOENT);
    assert(symlink("target", "") == -1 && errno == ENOENT);
    char file[] = "created-XXXXXX";
    int fd = mkostemp(file, O_CLOEXEC);
    assert(fd >= 0 && fcntl(fd, F_GETFD) & FD_CLOEXEC);
    assert(fstat(fd, &metadata) == 0 && (metadata.st_mode & 0777) == 0600);
    assert(close(fd) == 0 && chmod(file, 0640) == 0);
    assert(rename(file, "renamed") == 0);
    assert(access(file, F_OK) == -1 && errno == ENOENT);
    assert(rename("renamed", file) == 0);
    assert(chown(file, uid_t(-1), gid_t(-1)) == 0);
    assert(chown("missing", uid_t(-1), gid_t(-1)) == -1 && errno == ENOENT);
    assert(stat(file, &metadata) == 0 && (metadata.st_mode & 0777) == 0640);
    assert(link(file, "hard") == 0 && symlink(file, "symbolic") == 0);
    int directory = open(".", O_RDONLY | O_DIRECTORY);
    assert(directory >= 0);
    assert(linkat(directory, "symbolic", directory, "followed", AT_SYMLINK_FOLLOW) == 0);
    struct stat target{};
    assert(lstat("followed", &target) == 0 && S_ISREG(target.st_mode) && target.st_ino == metadata.st_ino);
    assert(linkat(directory, file, directory, "bad", 0x40000000) == -1 && errno == EINVAL);
    assert(linkat(-1, file, directory, "bad", 0) == -1 && errno == EBADF);
    assert(linkat(directory, file, -1, "bad", 0) == -1 && errno == EBADF);
    assert(renameat(-1, file, directory, "bad") == -1 && errno == EBADF);
    assert(renameat(directory, file, -1, "bad") == -1 && errno == EBADF);
    assert(symlinkat("target", -1, "bad") == -1 && errno == EBADF);
    assert(unlinkat(directory, file, 0x40000000) == -1 && errno == EINVAL);
    assert(unlink(".") == -1 && (errno == EISDIR || errno == EPERM));
    assert(symlink(file, "symbolic") == -1 && errno == EEXIST);
    assert(fstatat(AT_FDCWD, "", &target, AT_EMPTY_PATH) == 0 && S_ISDIR(target.st_mode));
    assert(fstatat(-1, "", &target, AT_EMPTY_PATH) == -1 && errno == EBADF);
    char buffer[256];
    assert(readlinkat(AT_FDCWD, "", buffer, sizeof(buffer)) == -1 && errno == ENOENT);
    assert(fstatat(-1, "file", &target, 0) == -1 && errno == EBADF);

    assert(mknod("fifo", S_IFIFO | 0600, 0) == 0);
    assert(mknodat(directory, "fifo-at", S_IFIFO | 0640, 0) == 0);
    assert(mkfifoat(directory, "fifo-extra", 0600) == 0);
    assert(mknodat(-1, "invalid", S_IFIFO | 0600, 0) == -1 && errno == EBADF);
    assert(mkfifoat(-1, "invalid", 0600) == -1 && errno == EBADF);
    assert(lstat("fifo-at", &metadata) == 0 && S_ISFIFO(metadata.st_mode));
    int fifo = open("fifo", O_RDWR | O_NONBLOCK);
    assert(fifo >= 0 && write(fifo, "abc", 3) == 3 && read(fifo, buffer, 3) == 3);
    assert(memcmp(buffer, "abc", 3) == 0 && close(fifo) == 0);
    assert(mkfifoat(directory, "fifo-extra", 0600) == -1 && errno == EEXIST);

    utimbuf times{123456, 234567};
    assert(utime(file, &times) == 0);
    assert(stat(file, &metadata) == 0 && metadata.st_atim.tv_sec == 123456 && metadata.st_mtim.tv_sec == 234567);
    timeval fileTimes[] = {{234567, 123456}, {345678, 654321}};
    assert(utimes(file, fileTimes) == 0);
    assert(stat(file, &metadata) == 0 && metadata.st_atim.tv_sec == 234567 && metadata.st_atim.tv_nsec == 123456000);
    assert(metadata.st_mtim.tv_sec == 345678 && metadata.st_mtim.tv_nsec == 654321000);
    assert(utimes("missing", fileTimes) == -1 && errno == ENOENT);
    assert(utimes(file, nullptr) == 0 && utime(file, &times) == 0);
    timeval linkTimes[] = {{345678, 123456}, {456789, 654321}};
    assert(lutimes("symbolic", linkTimes) == 0);
    assert(lstat("symbolic", &metadata) == 0 && metadata.st_mtim.tv_sec == 456789 && metadata.st_mtim.tv_nsec == 654321000);
    assert(stat(file, &metadata) == 0 && metadata.st_mtim.tv_sec == 234567);
    assert(utime(file, nullptr) == 0 && lutimes("symbolic", nullptr) == 0);
    timespec exact[] = {{567890, 7}, {678901, 9}};
    assert(utimensat(directory, "symbolic", exact, AT_SYMLINK_NOFOLLOW) == 0);
    assert(lstat("symbolic", &metadata) == 0 && metadata.st_mtim.tv_sec == 678901 && metadata.st_mtim.tv_nsec == 9);
    assert(utimensat(-1, file, exact, 0) == -1 && errno == EBADF);

    char suffix[] = "suffix-XXXXXX.txt";
    fd = mkstemps(suffix, 4);
    assert(fd >= 0 && strcmp(suffix + strlen(suffix) - 4, ".txt") == 0);
    assert(close(fd) == 0 && unlink(suffix) == 0);
    char unused[] = "unused-XXXXXX";
    assert(mktemp(unused) == unused && unused[0]);
    assert(access(unused, F_OK) == -1 && errno == ENOENT);
    char bad[] = "invalid";
    assert(mkstemp(bad) == -1 && errno == EINVAL);
    assert(mkdtemp(bad) == nullptr && errno == EINVAL);
    assert(mktemp(bad) == bad && bad[0] == 0 && errno == EINVAL);
    char absent[] = "absent/file-XXXXXX";
    assert(mkstemp(absent) == -1 && errno == ENOENT);
    assert(creat("absent/file", 0600) == -1 && errno == ENOENT);
    char absentDirectory[] = "absent/dir-XXXXXX";
    assert(mkdtemp(absentDirectory) == nullptr && errno == ENOENT);
    char notDirectory[256];
    assert(snprintf(notDirectory, sizeof(notDirectory), "%s/name-XXXXXX", file) > 0);
    errno = 0;
    assert(mktemp(notDirectory) == notDirectory && notDirectory[0] == 0);
    assert(errno == ENOTDIR);
    char longName[4097];
    memset(longName, 'x', sizeof(longName) - 1);
    longName[sizeof(longName) - 1] = 0;
    assert(open(longName, O_RDONLY) == -1 && errno == ENAMETOOLONG);
    assert(chdir(file) == -1 && errno == ENOTDIR);
    assert(opendir("missing") == nullptr && errno == ENOENT);
    dirent** entries = nullptr;
    assert(scandir(".", &entries, reject, nullptr) == 0);
    free(entries);
    assert(close(directory) == 0);
#ifdef __wasm__
    assert(chroot(".") == -1 && errno == EPERM);
    assert(acct(nullptr) == -1 && errno == EPERM);
    assert(canonicalize_file_name("missing") == nullptr && errno == ENOENT);
#endif
    umask(previous);
    puts("path operations: ok");
}
