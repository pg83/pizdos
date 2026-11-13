//@ compile-flags: -g -D_GNU_SOURCE
//@ stdout: "socket messages: ok\n"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <unistd.h>

int main() {
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0);
    assert(fcntl(pair[0], F_GETFD) & FD_CLOEXEC);
    int type = 0;
    socklen_t length = sizeof(type);
    assert(getsockopt(pair[0], SOL_SOCKET, SO_TYPE, &type, &length) == 0 && type == SOCK_STREAM && length == sizeof(type));
    int enabled = 1;
    assert(setsockopt(pair[0], SOL_SOCKET, SO_KEEPALIVE, &enabled, sizeof(enabled)) == 0);
    type = 0;
    length = sizeof(type);
    assert(getsockopt(pair[0], SOL_SOCKET, SO_KEEPALIVE, &type, &length) == 0 && type == 1);
    ucred credentials{};
    length = sizeof(credentials);
    assert(getsockopt(pair[0], SOL_SOCKET, SO_PEERCRED, &credentials, &length) == 0);
    assert(credentials.pid > 0 && credentials.uid == getuid() && credentials.gid == getgid());
    sockaddr_storage address{};
    length = sizeof(address);
    assert(getsockname(pair[0], reinterpret_cast<sockaddr*>(&address), &length) == 0 && address.ss_family == AF_UNIX);
    length = sizeof(address);
    assert(getpeername(pair[0], reinterpret_cast<sockaddr*>(&address), &length) == 0 && address.ss_family == AF_UNIX);
    char buffer[8];
    assert(send(pair[0], "peek", 4, 0) == 4);
    assert(recv(pair[1], buffer, 2, MSG_PEEK) == 2 && memcmp(buffer, "pe", 2) == 0);
    assert(recv(pair[1], buffer, 4, MSG_WAITALL) == 4 && memcmp(buffer, "peek", 4) == 0);
    assert(recv(pair[1], buffer, 1, MSG_DONTWAIT) == -1 && errno == EAGAIN);
    assert(sockatmark(pair[0]) == 0);
    int regular = open("/dev/null", O_RDONLY);
    assert(regular >= 0 && recv(regular, buffer, 1, 0) == -1 && errno == ENOTSOCK);
    assert(close(regular) == 0);
    assert(send(-1, buffer, 1, 0) == -1 && errno == EBADF);
    assert(shutdown(pair[0], 999) == -1 && errno == EINVAL);
    assert(accept(-1, nullptr, nullptr) == -1 && errno == EBADF);
    assert(accept4(pair[0], nullptr, nullptr, 0x100) == -1 && errno == EINVAL);

    int passed = open("/dev/zero", O_RDONLY);
    assert(passed >= 0);
    char first[] = "ab", second[] = "cd";
    iovec outgoingVectors[] = {{first, 2}, {second, 2}};
    alignas(cmsghdr) unsigned char control[CMSG_SPACE(sizeof(int))]{};
    msghdr outgoing{};
    outgoing.msg_iov = outgoingVectors;
    outgoing.msg_iovlen = 2;
    outgoing.msg_control = control;
    outgoing.msg_controllen = sizeof(control);
    cmsghdr* header = CMSG_FIRSTHDR(&outgoing);
    assert(header);
    header->cmsg_len = CMSG_LEN(sizeof(int));
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    memcpy(CMSG_DATA(header), &passed, sizeof(passed));
    assert(sendmsg(pair[0], &outgoing, 0) == 4);
    char left[2], right[2];
    iovec incomingVectors[] = {{left, 2}, {right, 2}};
    alignas(cmsghdr) unsigned char receivedControl[sizeof(control)]{};
    msghdr incoming{};
    incoming.msg_iov = incomingVectors;
    incoming.msg_iovlen = 2;
    incoming.msg_control = receivedControl;
    incoming.msg_controllen = sizeof(receivedControl);
    assert(recvmsg(pair[1], &incoming, 0) == 4 && incoming.msg_flags == 0);
    assert(memcmp(left, "ab", 2) == 0 && memcmp(right, "cd", 2) == 0);
    const cmsghdr* received = CMSG_FIRSTHDR(&incoming);
    assert(received && received->cmsg_level == SOL_SOCKET && received->cmsg_type == SCM_RIGHTS);
    assert(received->cmsg_len == CMSG_LEN(sizeof(int)) && CMSG_NXTHDR(&incoming, received) == nullptr);
    int installed;
    memcpy(&installed, CMSG_DATA(received), sizeof(installed));
    assert(installed >= 0 && fcntl(installed, F_GETFD) == 0);
    assert(read(installed, buffer, 1) == 1 && buffer[0] == 0 && close(installed) == 0);
    assert(sendmsg(pair[0], &outgoing, 0) == 4);
    incoming.msg_controllen = sizeof(receivedControl);
    assert(recvmsg(pair[1], &incoming, MSG_CMSG_CLOEXEC) == 4);
    received = CMSG_FIRSTHDR(&incoming);
    assert(received);
    memcpy(&installed, CMSG_DATA(received), sizeof(installed));
    assert(installed >= 0 && fcntl(installed, F_GETFD) == FD_CLOEXEC && close(installed) == 0);
    assert(sendmsg(pair[0], &outgoing, 0) == 4);
    incoming.msg_controllen = 1;
    assert(recvmsg(pair[1], &incoming, 0) == 4 && incoming.msg_flags & MSG_CTRUNC && incoming.msg_controllen == 0);
    header->cmsg_len = sizeof(cmsghdr) - 1;
    assert(sendmsg(pair[0], &outgoing, 0) == -1 && errno == EINVAL);
    header->cmsg_len = CMSG_LEN(sizeof(int));
    int missing = -1;
    memcpy(CMSG_DATA(header), &missing, sizeof(missing));
    assert(sendmsg(pair[0], &outgoing, 0) == -1 && errno == EBADF);
    memcpy(CMSG_DATA(header), &passed, sizeof(passed));
    outgoing.msg_iovlen = 1025;
    assert(sendmsg(pair[0], &outgoing, 0) == -1);
    outgoing.msg_iovlen = 2;
    rlimit original{};
    assert(getrlimit(RLIMIT_NOFILE, &original) == 0);
    rlimit limited = original;
    limited.rlim_cur = passed + 1;
    assert(setrlimit(RLIMIT_NOFILE, &limited) == 0);
    assert(sendmsg(pair[0], &outgoing, 0) == 4);
    incoming.msg_controllen = sizeof(receivedControl);
    assert(recvmsg(pair[1], &incoming, 0) == 4 && incoming.msg_flags & MSG_CTRUNC && incoming.msg_controllen == 0);
    assert(setrlimit(RLIMIT_NOFILE, &original) == 0);
    assert(close(passed) == 0);
    assert(shutdown(pair[0], SHUT_WR) == 0 && recv(pair[1], buffer, sizeof(buffer), 0) == 0);
    assert(send(pair[0], "x", 1, MSG_NOSIGNAL) == -1 && errno == EPIPE);
    assert(close(pair[0]) == 0 && close(pair[1]) == 0);

    assert(socketpair(AF_UNIX, SOCK_DGRAM, 0, pair) == 0);
    assert(send(pair[0], "abcd", 4, 0) == 4);
    iovec truncated{buffer, 2};
    incoming = {};
    incoming.msg_iov = &truncated;
    incoming.msg_iovlen = 1;
    assert(recvmsg(pair[1], &incoming, MSG_TRUNC) == 4 && incoming.msg_flags & MSG_TRUNC && memcmp(buffer, "ab", 2) == 0);
    timeval timeout{0, 20000}, actual{};
    assert(setsockopt(pair[1], SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    assert(setsockopt(pair[1], SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0);
    length = sizeof(actual);
    assert(getsockopt(pair[1], SOL_SOCKET, SO_RCVTIMEO, &actual, &length) == 0);
    assert(length == sizeof(actual) && actual.tv_sec == 0 && actual.tv_usec > 0 && actual.tv_usec <= 40000);
    length = sizeof(actual);
    assert(getsockopt(pair[1], SOL_SOCKET, SO_SNDTIMEO, &actual, &length) == 0 && actual.tv_usec > 0);
    assert(recv(pair[1], buffer, sizeof(buffer), 0) == -1 && errno == EAGAIN);
    assert(setsockopt(pair[1], SOL_SOCKET, SO_RCVTIMEO, &timeout, 1) == -1 && errno == EINVAL);
    timeout.tv_usec = 1000000;
    assert(setsockopt(pair[1], SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == -1);
    length = 1;
    assert(getsockopt(pair[1], SOL_SOCKET, SO_RCVTIMEO, buffer, &length) == 0 && length == 1);
    length = 0;
    assert(getsockopt(pair[1], SOL_SOCKET, SO_RCVTIMEO, nullptr, &length) == 0 && length == 0);
    assert(close(pair[0]) == 0 && close(pair[1]) == 0);

    assert(socketpair(AF_UNIX, SOCK_DGRAM, 0, pair) == 0);
    assert(setsockopt(pair[1], SOL_SOCKET, SO_PASSCRED, &enabled, sizeof(enabled)) == 0);
    alignas(cmsghdr) unsigned char credentialsControl[CMSG_SPACE(sizeof(ucred))];
    iovec credentialVector{buffer, sizeof(buffer)};
    const bool capacities[] = {true, false};
    for (bool shortBuffer : capacities) {
        assert(send(pair[0], "cred", 4, 0) == 4);
        memset(credentialsControl, 0x5a, sizeof(credentialsControl));
        incoming = {};
        incoming.msg_iov = &credentialVector;
        incoming.msg_iovlen = 1;
        incoming.msg_control = credentialsControl;
        incoming.msg_controllen = shortBuffer ? sizeof(cmsghdr) - 1 : sizeof(credentialsControl);
        assert(recvmsg(pair[1], &incoming, 0) == 4 && memcmp(buffer, "cred", 4) == 0);
        if (shortBuffer) {
            assert((incoming.msg_flags & MSG_CTRUNC) && incoming.msg_controllen == 0);
            for (unsigned char byte : credentialsControl) {
                assert(byte == 0x5a);
            }
        } else {
            received = CMSG_FIRSTHDR(&incoming);
            assert(received && received->cmsg_level == SOL_SOCKET && received->cmsg_type == SCM_CREDENTIALS);
            assert(received->cmsg_len == CMSG_LEN(sizeof(ucred)));
            memcpy(&credentials, CMSG_DATA(received), sizeof(credentials));
            assert(credentials.pid > 0 && credentials.uid == getuid() && credentials.gid == getgid());
        }
    }
    assert(close(pair[0]) == 0 && close(pair[1]) == 0);
    puts("socket messages: ok");
}
