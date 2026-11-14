//@ host: false
//@ compile-flags: -O2 -D_GNU_SOURCE
//@ stdout: "memory windows: ok\n"

// A memfd of a range of a process's memory, passed through three processes
// as a partition's block would go: A, the reader, seeks it to its window
// and passes it to B; B, as a partition, seeks further by its offset and
// passes it on to C; C, as the disk, writes there. The bytes land in A's
// memory at the window and nowhere else, B never has them, and the
// position the three share ends past them. A read-only grant refuses a
// write, a write-only one a read, and the memory outlives its process
// while a descriptor of it is open.
//
// With the argument bench it measures instead: a copy of the bytes in one
// process, the same bytes from C into A's window through the memfd, and
// the same bytes relayed C -> B -> A over seqpacket pairs.

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void sendFd(int sock, int fd) {
    char byte = 0;
    struct iovec iov = {&byte, 1};
    union {
        struct cmsghdr header;
        char space[CMSG_SPACE(sizeof(int))];
    } control = {};
    struct msghdr msg = {};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control.space;
    msg.msg_controllen = sizeof control.space;
    struct cmsghdr* c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &fd, sizeof fd);
    assert(sendmsg(sock, &msg, 0) == 1);
}

static int receiveFd(int sock) {
    char byte;
    struct iovec iov = {&byte, 1};
    union {
        struct cmsghdr header;
        char space[CMSG_SPACE(sizeof(int))];
    } control = {};
    struct msghdr msg = {};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control.space;
    msg.msg_controllen = sizeof control.space;
    assert(recvmsg(sock, &msg, 0) == 1);
    struct cmsghdr* c = CMSG_FIRSTHDR(&msg);
    assert(c && c->cmsg_type == SCM_RIGHTS);
    int fd;
    memcpy(&fd, CMSG_DATA(c), sizeof fd);
    return fd;
}

static void reap(pid_t child) {
    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static unsigned char pattern(size_t k) {
    return (unsigned char)(k * 7 + 1);
}

// changed is what A writes over the pattern before C reads it.
static unsigned char changed(size_t k) {
    return (unsigned char)(pattern(k) + 1);
}

static double now() {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

// chain passes fd from A through B, which seeks by delta, to C, which runs
// produce on it.
static void chain(int fd, off_t delta, void (*produce)(int)) {
    int ab[2], bc[2];
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, ab) == 0);
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, bc) == 0);
    pid_t b = fork();
    assert(b >= 0);
    if (b == 0) {
        int got = receiveFd(ab[1]);
        assert(lseek(got, delta, SEEK_CUR) >= delta);
        sendFd(bc[0], got);
        _exit(0);
    }
    pid_t c = fork();
    assert(c >= 0);
    if (c == 0) {
        produce(receiveFd(bc[1]));
        _exit(0);
    }
    sendFd(ab[0], fd);
    reap(b);
    reap(c);
    close(ab[0]);
    close(ab[1]);
    close(bc[0]);
    close(bc[1]);
}

static const size_t windowLength = 65536;

static void writeWindow(int fd) {
    unsigned char* data = (unsigned char*)malloc(windowLength);
    for (size_t k = 0; k < windowLength; ++k) {
        data[k] = pattern(k);
    }
    assert(write(fd, data, windowLength) == (ssize_t)windowLength);
}

static void readWindow(int fd) {
    unsigned char* data = (unsigned char*)malloc(windowLength);
    assert(read(fd, data, windowLength) == (ssize_t)windowLength);
    for (size_t k = 0; k < windowLength; ++k) {
        assert(data[k] == changed(k));
    }
}

static void check() {
    const size_t size = 1 << 20;
    const off_t base = 4096, delta = 512;
    unsigned char* buffer = (unsigned char*)malloc(size);
    memset(buffer, 0xaa, size);
    int fd = memfd_create(buffer, size, O_RDWR);
    assert(fd >= 0);
    struct stat st;
    assert(fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size == (off_t)size);

    // C writes at the window A and B set.
    assert(lseek(fd, base, SEEK_SET) == base);
    chain(fd, delta, writeWindow);
    for (size_t k = 0; k < size; ++k) {
        bool inside = k >= size_t(base + delta) && k < base + delta + windowLength;
        assert(buffer[k] == (inside ? pattern(k - base - delta) : 0xaa));
    }
    assert(lseek(fd, 0, SEEK_CUR) == base + delta + (off_t)windowLength);

    // C reads what A changed in place.
    for (size_t k = 0; k < windowLength; ++k) {
        buffer[base + delta + k] = changed(k);
    }
    assert(lseek(fd, base, SEEK_SET) == base);
    chain(fd, delta, readWindow);

    // The range does not grow; its end reads nothing.
    assert(pwrite(fd, "x", 1, size) == -1 && errno == ENOSPC);
    char byte;
    assert(pread(fd, &byte, 1, size) == 0);
    close(fd);

    // The grant is the access mode; a range must be the caller's memory.
    int readOnly = memfd_create(buffer, 16, O_RDONLY);
    assert(readOnly >= 0 && write(readOnly, "x", 1) == -1 && errno == EBADF);
    int writeOnly = memfd_create(buffer, 16, O_WRONLY);
    assert(writeOnly >= 0 && read(writeOnly, &byte, 1) == -1 && errno == EBADF);
    close(readOnly);
    close(writeOnly);
    assert(memfd_create(buffer, (size_t)1 << 31, O_RDWR) == -1 && errno == EFAULT);
    assert(memfd_create(buffer, 16, O_RDWR | O_APPEND) == -1 && errno == EINVAL);

    // A process's memory stays while a descriptor of it is open.
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair) == 0);
    pid_t owner = fork();
    assert(owner >= 0);
    if (owner == 0) {
        static char own[64] = "the memory of a process that ended";
        sendFd(pair[0], memfd_create(own, sizeof own, O_RDWR));
        _exit(0);
    }
    int orphan = receiveFd(pair[1]);
    reap(owner);
    char text[64];
    assert(pread(orphan, text, sizeof text, 0) == sizeof text && strcmp(text, "the memory of a process that ended") == 0);
    assert(pwrite(orphan, "still", 5, 0) == 5 && pread(orphan, text, 5, 0) == 5 && memcmp(text, "still", 5) == 0);
    close(orphan);
}

static const size_t benchLength = 256 << 20;
static unsigned char* benchData;

static void benchWindow(int fd) {
    double start = now();
    assert(write(fd, benchData, benchLength) == (ssize_t)benchLength);
    printf("window:  %.3f s\n", now() - start);
    fflush(stdout);
}

static void bench() {
    unsigned char* buffer = (unsigned char*)malloc(benchLength);
    benchData = (unsigned char*)malloc(benchLength);
    assert(buffer && benchData);
    for (size_t k = 0; k < benchLength; ++k) {
        benchData[k] = pattern(k);
    }
    memset(buffer, 0, benchLength);

    double start = now();
    memcpy(buffer, benchData, benchLength);
    printf("memcpy:  %.3f s\n", now() - start);
    memset(buffer, 0, benchLength);
    fflush(stdout);

    int fd = memfd_create(buffer, benchLength, O_RDWR);
    assert(fd >= 0);
    chain(fd, 0, benchWindow);
    assert(memcmp(buffer, benchData, benchLength) == 0);
    memset(buffer, 0, benchLength);

    // The same bytes as messages: C sends, B receives and sends on, A
    // receives.
    const size_t chunk = 32768;
    int cb[2], ba[2];
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, cb) == 0);
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, ba) == 0);
    pid_t b = fork();
    if (b == 0) {
        unsigned char* relay = (unsigned char*)malloc(chunk);
        for (size_t done = 0; done < benchLength; done += chunk) {
            assert(read(cb[1], relay, chunk) == (ssize_t)chunk);
            assert(write(ba[0], relay, chunk) == (ssize_t)chunk);
        }
        _exit(0);
    }
    pid_t c = fork();
    if (c == 0) {
        for (size_t done = 0; done < benchLength; done += chunk) {
            assert(write(cb[0], benchData + done, chunk) == (ssize_t)chunk);
        }
        _exit(0);
    }
    start = now();
    for (size_t done = 0; done < benchLength; done += chunk) {
        assert(read(ba[1], buffer + done, chunk) == (ssize_t)chunk);
    }
    printf("relay:   %.3f s\n", now() - start);
    reap(b);
    reap(c);
    assert(memcmp(buffer, benchData, benchLength) == 0);
}

int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "bench") == 0) {
        bench();
        return 0;
    }
    check();
    printf("memory windows: ok\n");
    return 0;
}
