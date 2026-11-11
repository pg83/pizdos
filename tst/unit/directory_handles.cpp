//@ compile-flags: -g -D_GNU_SOURCE
//@ stdout: "directory handles: ok\n"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

int main() {
    char work[] = "directories-XXXXXX";
    assert(mkdtemp(work) == work && chdir(work) == 0);
    assert(mkdir("empty", 0700) == 0 && remove("empty") == 0);
    assert(access("empty", F_OK) == -1 && errno == ENOENT);
    assert(mkdir("parent", 0700) == 0 && mkdir("parent/child", 0700) == 0);
    assert(remove("parent") == -1 && (errno == ENOTEMPTY || errno == EEXIST));
    assert(symlink("parent", "link") == 0 && remove("link") == 0);
    struct stat st{};
    assert(stat("parent", &st) == 0 && S_ISDIR(st.st_mode));
    assert(remove("parent/child") == 0 && remove("parent") == 0);
    assert(symlink("missing", "dangling") == 0 && remove("dangling") == 0);
    assert(remove("missing") == -1 && errno == ENOENT);

    DIR* stream = opendir(".");
    assert(stream && (fcntl(dirfd(stream), F_GETFD) & FD_CLOEXEC));
    assert(close(dirfd(stream)) == 0);
    errno = 0;
    assert(readdir(stream) == nullptr && errno == EBADF);
    struct dirent copied{}, *result = &copied;
    assert(readdir_r(stream, &copied, &result) == EBADF);
    assert(closedir(stream) == -1 && errno == EBADF);

    assert(mkdir("changed", 0700) == 0);
    int entry = open("changed/one", O_CREAT | O_WRONLY, 0600);
    assert(entry >= 0 && close(entry) == 0);
    entry = open("changed/two", O_CREAT | O_WRONLY, 0600);
    assert(entry >= 0 && close(entry) == 0);
    stream = opendir("changed");
    assert(stream);
    while (readdir(stream)) {
    }
    long end = telldir(stream);
    assert(unlink("changed/one") == 0 && unlink("changed/two") == 0);
    rewinddir(stream);
    seekdir(stream, end);
    assert(readdir(stream) == nullptr && closedir(stream) == 0 && rmdir("changed") == 0);

    int fd = open(".", O_RDONLY | O_DIRECTORY);
    assert(fd >= 0 && fcntl(fd, F_SETFD, 0) == 0);
    stream = fdopendir(fd);
    assert(stream && dirfd(stream) == fd);
    assert(fcntl(fd, F_GETFD) & FD_CLOEXEC);
    assert(readdir(stream) != nullptr && closedir(stream) == 0);
    assert(fcntl(fd, F_GETFD) == -1 && errno == EBADF);
    fd = open("regular", O_CREAT | O_RDWR, 0600);
    assert(fd >= 0);
    assert(fdopendir(fd) == nullptr && errno == ENOTDIR);
    assert(write(fd, "x", 1) == 1 && close(fd) == 0 && remove("regular") == 0);

    rlimit saved{};
    assert(getrlimit(RLIMIT_NOFILE, &saved) == 0);
    rlimit limit = saved;
    limit.rlim_cur = 3;
    assert(setrlimit(RLIMIT_NOFILE, &limit) == 0);
    assert(opendir(".") == nullptr && errno == EMFILE);
    assert(setrlimit(RLIMIT_NOFILE, &saved) == 0);
    stream = opendir(".");
    assert(stream && closedir(stream) == 0);
    assert(chdir("..") == 0 && remove(work) == 0);
    puts("directory handles: ok");
}
