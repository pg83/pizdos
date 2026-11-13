#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int attempt(const char* operation) {
    if (strcmp(operation, "system") == 0) {
        // A shell that cannot be loaded ends with 127, as system reports.
        int status = system("exit7");
        if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
            return 1;
        }
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 7);
        return 0;
    }
    if (strcmp(operation, "daemon") == 0) {
        int saved[3];
        for (int fd = 0; fd < 3; ++fd) {
            saved[fd] = dup(fd);
            assert(saved[fd] >= 0);
        }
        int result = daemon(1, 0), error = errno;
        for (int fd = 0; fd < 3; ++fd) {
            assert(dup2(saved[fd], fd) == fd && close(saved[fd]) == 0);
        }
        assert(result == 0 || (result == -1 && error == EIO));
        return result < 0;
    }
    if (strcmp(operation, "forkpty") == 0) {
        int master = -1;
        pid_t child = forkpty(&master, NULL, NULL, NULL);
        if (child < 0) {
            assert(errno == EAGAIN && master == -1);
            return 1;
        }
        if (child == 0) {
            _exit(7);
        }
        int status;
        assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 7);
        assert(close(master) == 0);
        return 0;
    }
    if (strcmp(operation, "chdir") == 0) {
        int result = chdir("/");
        assert(result == 0 || (result == -1 && errno == EIO));
        return result < 0;
    }
    if (strcmp(operation, "relative-open") == 0 || strcmp(operation, "fexecve") == 0) {
        int execute = strcmp(operation, "fexecve") == 0;
        int fd = open(execute ? "/dev/null" : "/", O_RDONLY);
        assert(fd >= 0);
        int result;
        if (execute) {
            char* args[] = {"null", NULL};
            result = fexecve(fd, args, NULL);
        } else {
            result = openat(fd, "dev/null", O_RDONLY);
        }
        int failed = result < 0 && errno == EIO;
        assert(failed || (execute ? result == -1 && errno == EACCES : result >= 0));
        if (!execute && result >= 0) {
            assert(close(result) == 0);
        }
        assert(close(fd) == 0);
        return failed;
    }
    if (strcmp(operation, "ttyname") == 0) {
        int master, slave;
        assert(openpty(&master, &slave, NULL, NULL, NULL) == 0);
        char name[128];
        memset(name, 0x5a, sizeof(name));
        int error = ttyname_r(slave, name, sizeof(name));
        assert(error == 0 || error == EIO);
        if (error != 0) {
            for (unsigned i = 0; i < sizeof(name); ++i) {
                assert(name[i] == 0x5a);
            }
        } else {
            assert(strncmp(name, "/dev/pts/", 9) == 0);
        }
        assert(close(master) == 0 && close(slave) == 0);
        return error != 0;
    }
    if (strcmp(operation, "fstat") == 0 || strcmp(operation, "fstatat") == 0 || strcmp(operation, "stat") == 0 || strcmp(operation, "lstat") == 0) {
        struct stat metadata;
        memset(&metadata, 0x5a, sizeof(metadata));
        int result;
        if (strcmp(operation, "fstat") == 0) {
            int fd = open("/dev/null", O_RDONLY);
            assert(fd >= 0);
            result = fstat(fd, &metadata);
            int error = errno;
            assert(close(fd) == 0);
            errno = error;
        } else if (strcmp(operation, "fstatat") == 0) {
            result = fstatat(AT_FDCWD, "/dev/null", &metadata, 0);
        } else if (strcmp(operation, "stat") == 0) {
            result = stat("/dev/null", &metadata);
        } else {
            result = lstat("/dev/null", &metadata);
        }
        if (result < 0) {
            assert(errno == EIO);
            const unsigned char* bytes = (const unsigned char*)&metadata;
            for (unsigned i = 0; i < sizeof(metadata); ++i) {
                assert(bytes[i] == 0x5a);
            }
            return 1;
        }
        assert(result == 0 && S_ISCHR(metadata.st_mode));
        return 0;
    }
    if (strcmp(operation, "fdopendir") == 0) {
        int fd = open("/", O_RDONLY | O_DIRECTORY);
        assert(fd >= 0);
        DIR* dir = fdopendir(fd);
        if (dir == NULL) {
            assert(errno == EIO && close(fd) == 0);
            return 1;
        }
        assert(closedir(dir) == 0);
        return 0;
    }
    if (strcmp(operation, "msync") == 0) {
        int fd = open("/mapping", O_CREAT | O_TRUNC | O_RDWR, 0600);
        assert(fd >= 0 && ftruncate(fd, 4096) == 0);
        char* mapping = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        assert(mapping != MAP_FAILED);
        mapping[0] = 'x';
        int result = msync(mapping, 4096, MS_SYNC);
        int failed = result < 0;
        assert(failed ? errno == EIO : result == 0);
        assert(mapping[0] == 'x' && munmap(mapping, 4096) == 0);
        char byte;
        assert(pread(fd, &byte, 1, 0) == 1 && byte == 'x');
        assert(close(fd) == 0 && unlink("/mapping") == 0);
        return failed;
    }
    if (strcmp(operation, "memory-grow") == 0) {
        size_t before = __builtin_wasm_memory_size(0);
        size_t old = __builtin_wasm_memory_grow(0, 1);
        if (old == (size_t)-1) {
            assert(__builtin_wasm_memory_size(0) == before);
            return 1;
        }
        assert(old == before && __builtin_wasm_memory_size(0) == before + 1);
        volatile char* last = (volatile char*)((before + 1) * 65536 - 1);
        *last = 'x';
        assert(*last == 'x');
        return 0;
    }
    if (strcmp(operation, "fork") == 0) {
        pid_t child = fork();
        if (child < 0) {
            assert(errno == EAGAIN);
            return 1;
        }
        if (child == 0) {
            _exit(7);
        }
        int status;
        assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 7);
        return 0;
    }
    if (strcmp(operation, "openat") == 0) {
        int fd = open("/dev/null", O_RDONLY);
        if (fd < 0) {
            assert(errno == EIO);
            return 1;
        }
        assert(close(fd) == 0);
        return 0;
    }
    if (strcmp(operation, "read") == 0) {
        char byte;
        ssize_t size = read(0, &byte, 1);
        if (size < 0) {
            assert(errno == EIO);
            return 1;
        }
        assert(size == 1 && byte == 'x');
        return 0;
    }
    assert(strcmp(operation, "write") == 0);
    int fd = open("/dev/null", O_WRONLY);
    assert(fd >= 0);
    ssize_t size = write(fd, "x", 1);
    int failed = size < 0;
    assert(failed ? errno == EIO : size == 1);
    assert(close(fd) == 0);
    return failed;
}

int main(int argc, char** argv) {
    if (argc == 3 && strcmp(argv[1], "-c") == 0 && strcmp(argv[2], "exit7") == 0) {
        return 7;
    }
    assert(argc == 2);
    int failures = 0;
    for (int i = 0; i < 16; ++i) {
        failures += attempt(argv[1]);
    }
    printf("%s: failures=%d\n", argv[1], failures);
    return 0;
}
