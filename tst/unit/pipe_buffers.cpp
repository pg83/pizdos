//@ compile-flags: -g -D_GNU_SOURCE
//@ stdout: "pipe buffers: ok\n"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

// A pipe holds 64 KiB: small writes merge into it, a write of at most
// PIPE_BUF bytes goes whole or not at all, a larger one as far as the room
// goes, and a reader drains what writers left before it sees their end.

static const int capacity = 65536;
static char buffer[2 * 65536];

static void drain(int fd, int expected) {
    int total = 0;
    while (total < expected) {
        ssize_t n = read(fd, buffer, sizeof(buffer));
        assert(n > 0);
        total += (int)n;
    }
    assert(total == expected);
    assert(read(fd, buffer, 1) == -1 && errno == EAGAIN);
}

int main() {
    int ends[2];
    assert(pipe2(ends, O_NONBLOCK) == 0);
    struct stat metadata;
    assert(fstat(ends[0], &metadata) == 0 && S_ISFIFO(metadata.st_mode));
    assert(fstat(ends[1], &metadata) == 0 && S_ISFIFO(metadata.st_mode));
    assert(lseek(ends[0], 0, SEEK_SET) == -1 && errno == ESPIPE);
    assert(write(ends[0], "x", 1) == -1 && errno == EBADF);
    assert(read(ends[1], buffer, 1) == -1 && errno == EBADF);
    assert(read(ends[0], buffer, 0) == 0 && write(ends[1], buffer, 0) == 0);

    // Single bytes fill the pipe to its capacity; one more waits.
    for (int i = 0; i < capacity; ++i) {
        assert(write(ends[1], "b", 1) == 1);
    }
    int available = 0;
    assert(ioctl(ends[0], FIONREAD, &available) == 0 && available == capacity);
    assert(write(ends[1], "b", 1) == -1 && errno == EAGAIN);
    pollfd out = {ends[1], POLLOUT, 0};
    assert(poll(&out, 1, 0) == 0);
    drain(ends[0], capacity);
    assert(poll(&out, 1, 0) == 1 && (out.revents & POLLOUT));

    // Whole chunks of PIPE_BUF fill it too; a chunk goes whole or not at all.
    memset(buffer, 'c', sizeof(buffer));
    for (int i = 0; i < capacity / PIPE_BUF; ++i) {
        assert(write(ends[1], buffer, PIPE_BUF) == PIPE_BUF);
    }
    assert(write(ends[1], buffer, PIPE_BUF) == -1 && errno == EAGAIN);
    assert(write(ends[1], buffer, 1) == -1 && errno == EAGAIN);
    drain(ends[0], capacity);

    // A write larger than the pipe goes as far as the room: the rest waits.
    assert(write(ends[1], buffer, sizeof(buffer)) == capacity);
    assert(write(ends[1], buffer, sizeof(buffer)) == -1 && errno == EAGAIN);
    drain(ends[0], capacity);

    // A short read leaves the rest of what was written for the next.
    assert(write(ends[1], "hello", 5) == 5 && write(ends[1], "world", 5) == 5);
    assert(read(ends[0], buffer, 3) == 3 && memcmp(buffer, "hel", 3) == 0);
    assert(read(ends[0], buffer, 100) == 7 && memcmp(buffer, "loworld", 7) == 0);

    // The data outlives its writer; then the end is seen, and a closed
    // reader breaks a write.
    assert(write(ends[1], "last", 4) == 4 && close(ends[1]) == 0);
    pollfd in = {ends[0], POLLIN, 0};
    assert(poll(&in, 1, 0) == 1 && (in.revents & POLLIN) && (in.revents & POLLHUP));
    assert(read(ends[0], buffer, 100) == 4 && memcmp(buffer, "last", 4) == 0);
    assert(read(ends[0], buffer, 100) == 0);
    assert(close(ends[0]) == 0);

    // A blocking writer and reader in two processes move more than the
    // pipe holds, in order.
    assert(pipe(ends) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        close(ends[1]);
        unsigned sum = 0;
        int total = 0;
        ssize_t n;
        while ((n = read(ends[0], buffer, 1000)) > 0) {
            for (ssize_t i = 0; i < n; ++i) {
                sum = sum * 31 + (unsigned char)buffer[i];
            }
            total += (int)n;
        }
        assert(n == 0 && total == 10 * capacity);
        _exit(sum == 1464139776u ? 0 : 1);
    }
    close(ends[0]);
    unsigned sum = 0;
    for (int i = 0; i < 10 * capacity; ++i) {
        buffer[i % capacity] = (char)(i * 7);
        sum = sum * 31 + (unsigned char)buffer[i % capacity];
        if (i % capacity == capacity - 1) {
            int written = 0;
            while (written < capacity) {
                ssize_t n = write(ends[1], buffer + written, capacity - written);
                assert(n > 0);
                written += (int)n;
            }
        }
    }
    assert(sum == 1464139776u);
    assert(close(ends[1]) == 0);
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    puts("pipe buffers: ok");
}
