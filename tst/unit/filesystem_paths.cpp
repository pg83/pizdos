//@ compile-flags: -g -D_GNU_SOURCE
//@ stdout: "filesystem paths: ok\n"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int entryName(const dirent* entry) {
    return strncmp(entry->d_name, "entry", 5) == 0;
}

static void touch(const char* path) {
    int fd = creat(path, 0600);
    assert(fd >= 0 && close(fd) == 0);
}

int main() {
    assert(mkdir("path-case", 0700) == 0 && chdir("path-case") == 0);
    char current[1024], buffer[1100];
    assert(getcwd(current, sizeof(current)) == current);
    assert(strcmp(strrchr(current, '/') + 1, "path-case") == 0);
    assert(getcwd(buffer, 1) == nullptr && errno == ERANGE);
    char* allocated = getcwd(nullptr, 0);
    assert(allocated && strcmp(allocated, current) == 0);
    free(allocated);
    allocated = get_current_dir_name();
    assert(allocated && strcmp(allocated, current) == 0);
    free(allocated);
    assert(setenv("PWD", current, 1) == 0);
    allocated = get_current_dir_name();
    assert(allocated && strcmp(allocated, current) == 0);
    free(allocated);
    assert(setenv("PWD", "/missing", 1) == 0);
    allocated = get_current_dir_name();
    assert(allocated && strcmp(allocated, current) == 0);
    free(allocated);
    int directory = open(".", O_RDONLY | O_DIRECTORY);
    assert(directory >= 0 && mkdirat(directory, "sub", 0700) == 0);
    touch("file");
    assert(symlinkat("../file", directory, "sub/link") == 0);
    int subdirectory = openat(directory, "sub", O_RDONLY | O_DIRECTORY);
    assert(subdirectory >= 0);
    memset(buffer, 'x', sizeof(buffer));
    assert(readlinkat(subdirectory, "link", buffer, sizeof(buffer)) == 7);
    assert(memcmp(buffer, "../file", 7) == 0 && buffer[7] == 'x');
    assert(readlinkat(subdirectory, "link", buffer, 3) == 3 && memcmp(buffer, "../", 3) == 0);
    assert(readlinkat(-1, "link", buffer, sizeof(buffer)) == -1 && errno == EBADF);
    ssize_t regularLink = readlink("file", buffer, sizeof(buffer));
    if (regularLink != -1 || errno != EINVAL) {
        fprintf(stderr, "readlink regular file: result=%lld errno=%d\n", (long long)regularLink, errno);
        abort();
    }
    assert(readlinkat(directory, "file", buffer, sizeof(buffer)) == -1 && errno == EINVAL);
    int regularPath = open("file", O_PATH);
    assert(regularPath >= 0);
    assert(readlinkat(regularPath, "", buffer, sizeof(buffer)) == -1 && errno == ENOENT);
    assert(close(regularPath) == 0);
    int symlinkPath = openat(subdirectory, "link", O_PATH | O_NOFOLLOW);
    assert(symlinkPath >= 0 && readlinkat(symlinkPath, "", buffer, sizeof(buffer)) == 7);
    assert(memcmp(buffer, "../file", 7) == 0 && close(symlinkPath) == 0);
    struct stat linkMetadata{}, fileMetadata{};
    assert(fstatat(subdirectory, "link", &linkMetadata, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(linkMetadata.st_mode));
    assert(fstatat(subdirectory, "link", &fileMetadata, 0) == 0 && S_ISREG(fileMetadata.st_mode));
    assert(lstat("sub/link", &linkMetadata) == 0 && S_ISLNK(linkMetadata.st_mode));
    assert(linkat(directory, "file", subdirectory, "hard", 0) == 0);
    assert(fstatat(subdirectory, "hard", &linkMetadata, 0) == 0 && linkMetadata.st_ino == fileMetadata.st_ino && linkMetadata.st_nlink == 2);
    assert(fchmodat(directory, "file", 0640, 0) == 0);
    assert(fstatat(directory, "file", &fileMetadata, 0) == 0 && (fileMetadata.st_mode & 0777) == 0640);
    assert(fchownat(directory, "file", uid_t(-1), gid_t(-1), 0) == 0);
    assert(lchown("sub/link", uid_t(-1), gid_t(-1)) == 0);
    assert(renameat(subdirectory, "hard", directory, "renamed") == 0);
    assert(faccessat(directory, "renamed", F_OK, 0) == 0);
    assert(eaccess("renamed", R_OK) == 0 && euidaccess("renamed", R_OK) == 0);
    assert(faccessat(directory, "missing", F_OK, 0) == -1 && errno == ENOENT);
    char expected[1100];
    snprintf(expected, sizeof(expected), "%s/file", current);
    assert(realpath("sub/link", buffer) == buffer && strcmp(buffer, expected) == 0);
    allocated = realpath("sub/../file", nullptr);
    assert(allocated && strcmp(allocated, expected) == 0);
    free(allocated);
#ifdef __wasm__
    allocated = canonicalize_file_name("sub/link");
    assert(allocated && strcmp(allocated, expected) == 0);
    free(allocated);
#endif
    assert(realpath("missing", buffer) == nullptr && errno == ENOENT);
    assert(symlink("loop", "loop") == 0);
    assert(realpath("loop", buffer) == nullptr && errno == ELOOP);
    assert(chdir("sub") == 0 && fchdir(directory) == 0);
    assert(getcwd(buffer, sizeof(buffer)) == buffer && strcmp(buffer, current) == 0);
    assert(fchdir(-1) == -1 && errno == EBADF);
    touch("entry2");
    touch("entry10");
    touch("entry1");
    DIR* stream = opendir(".");
    assert(stream && dirfd(stream) >= 0);
    long start = telldir(stream);
    dirent* entry = readdir(stream);
    assert(entry && strlen(entry->d_name) < sizeof(buffer));
    strcpy(buffer, entry->d_name);
    seekdir(stream, start);
    entry = readdir(stream);
    assert(entry && strcmp(entry->d_name, buffer) == 0);
    rewinddir(stream);
    unsigned entries = 0;
    while (readdir(stream)) {
        entries++;
    }
    assert(entries == 9 && readdir(stream) == nullptr);
    assert(closedir(stream) == 0);
    int owned = open(".", O_RDONLY | O_DIRECTORY);
    assert(owned >= 0);
    stream = fdopendir(owned);
    assert(stream && dirfd(stream) == owned && fcntl(owned, F_GETFD) & FD_CLOEXEC);
    struct dirent copied{}, *pointed = nullptr;
    assert(readdir_r(stream, &copied, &pointed) == 0 && pointed == &copied);
    assert(closedir(stream) == 0 && fcntl(owned, F_GETFD) == -1 && errno == EBADF);
    assert(fdopendir(-1) == nullptr && errno == EBADF);
    int regular = open("file", O_RDONLY);
    assert(regular >= 0 && fdopendir(regular) == nullptr && errno == ENOTDIR && close(regular) == 0);
    assert(opendir("file") == nullptr && errno == ENOTDIR);
    assert(opendir("missing") == nullptr && errno == ENOENT);
    dirent** list = nullptr;
    assert(scandir(".", &list, entryName, alphasort) == 3);
    const char* alphabetical[] = {"entry1", "entry10", "entry2"};
    for (int i = 0; i < 3; ++i) {
        assert(strcmp(list[i]->d_name, alphabetical[i]) == 0);
        free(list[i]);
    }
    free(list);
    assert(scandir(".", &list, entryName, versionsort) == 3);
    const char* versions[] = {"entry1", "entry2", "entry10"};
    for (int i = 0; i < 3; ++i) {
        assert(strcmp(list[i]->d_name, versions[i]) == 0);
        free(list[i]);
    }
    free(list);
    assert(unlinkat(subdirectory, "link", 0) == 0);
    assert(unlinkat(directory, "sub", AT_REMOVEDIR) == 0);
    assert(close(subdirectory) == 0 && close(directory) == 0);
    assert(rmdir("file") == -1 && errno == ENOTDIR);
    puts("filesystem paths: ok");
}
