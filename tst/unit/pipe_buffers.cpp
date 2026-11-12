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

// POSIX's pipe: a write of at most PIPE_BUF bytes goes whole or not at
// all, a full pipe takes nothing until it is read, a larger write goes as
// far as it can, and a reader drains what writers left before it sees
// their end. The capacity is the implementation's: filled until EAGAIN.

static char buffer[1 << 20];

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

// fill writes units of size until the pipe takes no more, and tells how
// many bytes it took.
static int fill(int fd, int size) {
    int total = 0;
    for (;;) {
        ssize_t n = write(fd, buffer, size);
        if (n < 0) {
            assert(errno == EAGAIN);
            return total;
        }
        assert(n == size);
        total += size;
        assert(total < (int)sizeof(buffer));
    }
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

    // Whole writes of PIPE_BUF fill the pipe; an empty pipe takes at least
    // one; a full one takes nothing, not a byte, and is not writable.
    memset(buffer, 'c', sizeof(buffer));
    int held = fill(ends[1], PIPE_BUF);
    assert(held >= PIPE_BUF && held % PIPE_BUF == 0);
    int available = 0;
    assert(ioctl(ends[0], FIONREAD, &available) == 0 && available == held);
    assert(write(ends[1], buffer, 1) == -1 && errno == EAGAIN);
    pollfd out = {ends[1], POLLOUT, 0};
    assert(poll(&out, 1, 0) == 0);
    drain(ends[0], held);
    assert(poll(&out, 1, 0) == 1 && (out.revents & POLLOUT));

    // Single bytes fill it too, however many it holds.
    held = fill(ends[1], 1);
    assert(held >= 1);
    assert(ioctl(ends[0], FIONREAD, &available) == 0 && available == held);
    drain(ends[0], held);

    // A write larger than the pipe goes as far as the room: the rest waits.
    ssize_t part = write(ends[1], buffer, sizeof(buffer));
    assert(part > 0 && part < (ssize_t)sizeof(buffer));
    assert(write(ends[1], buffer, sizeof(buffer)) == -1 && errno == EAGAIN);
    drain(ends[0], (int)part);

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
    const int capacity = 65536;
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
