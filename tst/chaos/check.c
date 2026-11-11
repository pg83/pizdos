#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int attempt(const char* operation) {
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
    assert(argc == 2);
    int failures = 0;
    for (int i = 0; i < 16; ++i) {
        failures += attempt(argv[1]);
    }
    printf("%s: failures=%d\n", argv[1], failures);
    return 0;
}
