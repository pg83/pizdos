//@ compile-flags: -g -D_GNU_SOURCE
//@ stdout: "seqpacket pairs: ok\n"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

// A SOCK_SEQPACKET pair keeps packet boundaries, passes descriptors with
// SCM_RIGHTS, pipes among them, and ends with EOF and EPIPE.

static volatile sig_atomic_t broken;

static void pipeBroken(int) {
    broken = broken + 1;
}

static int sendWith(int sock, const char* text, int fd) {
    iovec vector = {const_cast<char*>(text), strlen(text)};
    alignas(cmsghdr) unsigned char control[CMSG_SPACE(sizeof(int))] = {};
    msghdr message = {};
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    if (fd >= 0) {
        message.msg_control = control;
        message.msg_controllen = sizeof(control);
        cmsghdr* header = CMSG_FIRSTHDR(&message);
        header->cmsg_len = CMSG_LEN(sizeof(int));
        header->cmsg_level = SOL_SOCKET;
        header->cmsg_type = SCM_RIGHTS;
        memcpy(CMSG_DATA(header), &fd, sizeof(fd));
    }
    return (int)sendmsg(sock, &message, 0);
}

static int receiveWith(int sock, char* buffer, size_t size, int* fd, int* flags) {
    iovec vector = {buffer, size};
    alignas(cmsghdr) unsigned char control[CMSG_SPACE(sizeof(int))] = {};
    msghdr message = {};
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    int n = (int)recvmsg(sock, &message, MSG_CMSG_CLOEXEC);
    *fd = -1;
    *flags = message.msg_flags;
    for (cmsghdr* header = CMSG_FIRSTHDR(&message); header; header = CMSG_NXTHDR(&message, header)) {
        if (header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_RIGHTS) {
            memcpy(fd, CMSG_DATA(header), sizeof(*fd));
        }
    }
    return n;
}

int main() {
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair) == 0);
    assert(fcntl(pair[0], F_GETFD) & FD_CLOEXEC);
    struct stat metadata;
    assert(fstat(pair[0], &metadata) == 0 && S_ISSOCK(metadata.st_mode));
    int type = 0;
    socklen_t length = sizeof(type);
    assert(getsockopt(pair[0], SOL_SOCKET, SO_TYPE, &type, &length) == 0 && type == SOCK_SEQPACKET);
    assert(lseek(pair[0], 0, SEEK_SET) == -1 && errno == ESPIPE);

    // Packets keep their boundaries: two writes are two reads, and a
    // short read drops the rest of its packet.
    char buffer[32];
    assert(write(pair[0], "first", 5) == 5 && write(pair[0], "second", 6) == 6);
    assert(recv(pair[1], buffer, sizeof(buffer), MSG_PEEK) == 5 && memcmp(buffer, "first", 5) == 0);
    assert(read(pair[1], buffer, 2) == 2 && memcmp(buffer, "fi", 2) == 0);
    assert(read(pair[1], buffer, sizeof(buffer)) == 6 && memcmp(buffer, "second", 6) == 0);
    assert(recv(pair[1], buffer, 1, MSG_DONTWAIT) == -1 && errno == EAGAIN);
    pollfd input = {pair[1], POLLIN, 0};
    assert(poll(&input, 1, 0) == 0);
    pollfd output = {pair[0], POLLOUT, 0};
    assert(poll(&output, 1, 0) == 1 && (output.revents & POLLOUT));

    // A pipe of the kernel passes through the pair, and reads on the
    // other side what was written on this one.
    int ends[2];
    assert(pipe(ends) == 0);
    assert(sendWith(pair[0], "pipe", ends[0]) == 4);
    assert(close(ends[0]) == 0);
    int received = -1, flags = 0;
    assert(receiveWith(pair[1], buffer, sizeof(buffer), &received, &flags) == 4 && memcmp(buffer, "pipe", 4) == 0);
    assert(received >= 0 && (flags & ~MSG_CMSG_CLOEXEC) == 0 && (fcntl(received, F_GETFD) & FD_CLOEXEC));
    assert(write(ends[1], "through", 7) == 7);
    assert(read(received, buffer, sizeof(buffer)) == 7 && memcmp(buffer, "through", 7) == 0);
    assert(close(received) == 0 && close(ends[1]) == 0);

    // Without a control buffer the passed descriptor is dropped.
    assert(pipe(ends) == 0);
    assert(sendWith(pair[0], "dropped", ends[1]) == 7);
    assert(read(pair[1], buffer, sizeof(buffer)) == 7);
    assert(close(ends[1]) == 0);
    assert(read(ends[0], buffer, 1) == 0 && close(ends[0]) == 0);

    // A child answers through the pair; its end closes with it, and the
    // parent reads EOF after the answer.
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        close(pair[0]);
        char question[32];
        ssize_t n = read(pair[1], question, sizeof(question));
        if (n != 8 || memcmp(question, "question", 8) != 0) {
            _exit(1);
        }
        _exit(write(pair[1], "answer", 6) == 6 ? 0 : 2);
    }
    assert(close(pair[1]) == 0);
    assert(write(pair[0], "question", 8) == 8);
    assert(read(pair[0], buffer, sizeof(buffer)) == 6 && memcmp(buffer, "answer", 6) == 0);
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(read(pair[0], buffer, sizeof(buffer)) == 0);
    assert(poll(&output, 1, 0) == 1 && (output.revents & POLLHUP));
    // EPIPE, and no SIGPIPE: Linux sends none for a packet socket.
    assert(signal(SIGPIPE, pipeBroken) != SIG_ERR);
    assert(write(pair[0], "x", 1) == -1 && errno == EPIPE && broken == 0);
    assert(send(pair[0], "x", 1, MSG_NOSIGNAL) == -1 && errno == EPIPE && broken == 0);
    assert(close(pair[0]) == 0);

    // shutdown of writing ends the other side's reads.
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair) == 0);
    assert(shutdown(pair[0], SHUT_WR) == 0);
    assert(read(pair[1], buffer, 1) == 0);
    assert(write(pair[1], "back", 4) == 4 && read(pair[0], buffer, sizeof(buffer)) == 4);
    assert(close(pair[0]) == 0 && close(pair[1]) == 0);
    puts("seqpacket pairs: ok");
}
