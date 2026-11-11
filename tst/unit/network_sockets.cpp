//@ compile-flags: -g -D_GNU_SOURCE
//@ stdout: "network sockets: ok\n"

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static sockaddr_in bound(int fd) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    socklen_t length = sizeof(address);
    assert(getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) == 0 && length == sizeof(address));
    assert(address.sin_family == AF_INET && address.sin_port != 0 && address.sin_addr.s_addr == htonl(INADDR_LOOPBACK));
    return address;
}

int main() {
    assert(socket(-1, SOCK_STREAM, 0) == -1 && errno == EAFNOSUPPORT);
    assert(socket(AF_INET, -1, 0) == -1 && errno == EINVAL);
    int listener = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    assert(listener >= 0 && (fcntl(listener, F_GETFD) & FD_CLOEXEC));
    sockaddr_in address = bound(listener), peer{};
    socklen_t length = sizeof(peer);
    assert(getpeername(listener, reinterpret_cast<sockaddr*>(&peer), &length) == -1 && errno == ENOTCONN);
    int refused = socket(AF_INET, SOCK_STREAM, 0);
    assert(refused >= 0 && connect(refused, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == -1 && errno == ECONNREFUSED);
    assert(close(refused) == 0);
    assert(listen(listener, 4) == 0);
    assert(accept(listener, nullptr, nullptr) == -1 && (errno == EAGAIN || errno == EWOULDBLOCK));
    int client = socket(AF_INET, SOCK_STREAM, 0);
    assert(client >= 0 && connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    length = sizeof(peer);
    int accepted = accept4(listener, reinterpret_cast<sockaddr*>(&peer), &length, SOCK_CLOEXEC | SOCK_NONBLOCK);
    assert(accepted >= 0 && length == sizeof(peer) && peer.sin_family == AF_INET);
    assert((fcntl(accepted, F_GETFD) & FD_CLOEXEC) && (fcntl(accepted, F_GETFL) & O_NONBLOCK));
    sockaddr_in observed{};
    length = sizeof(observed);
    assert(getsockname(client, reinterpret_cast<sockaddr*>(&observed), &length) == 0 && observed.sin_port == peer.sin_port);
    length = sizeof(observed);
    assert(getpeername(client, reinterpret_cast<sockaddr*>(&observed), &length) == 0 && observed.sin_port == address.sin_port);
    assert(sockatmark(accepted) == 0);
    assert(sockatmark(-1) == -1 && errno == EBADF);
    assert(send(client, "hello", 5, 0) == 5);
    pollfd ready{accepted, POLLIN, 0};
    assert(poll(&ready, 1, 1000) == 1);
    char text[32];
    assert(recv(accepted, text, sizeof(text), 0) == 5 && memcmp(text, "hello", 5) == 0);
    assert(send(accepted, "ack", 3, 0) == 3);
    assert(recv(client, text, 3, MSG_WAITALL) == 3 && memcmp(text, "ack", 3) == 0);
    assert(close(client) == 0 && close(accepted) == 0);

    client = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    assert(client >= 0);
    int connected = connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    assert(connected == 0 || connected == -1 && errno == EINPROGRESS);
    ready = {client, POLLOUT, 0};
    assert(poll(&ready, 1, 1000) == 1);
    int error = -1;
    length = sizeof(error);
    assert(getsockopt(client, SOL_SOCKET, SO_ERROR, &error, &length) == 0 && error == 0);
    accepted = accept(listener, nullptr, nullptr);
    assert(accepted >= 0 && close(accepted) == 0 && close(client) == 0 && close(listener) == 0);

    int server = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    client = socket(AF_INET, SOCK_DGRAM, 0);
    assert(server >= 0 && client >= 0);
    address = bound(server);
    assert(sendto(client, "datagram", 8, 0, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 8);
    length = sizeof(peer);
    assert(recvfrom(server, text, sizeof(text), 0, reinterpret_cast<sockaddr*>(&peer), &length) == 8);
    assert(memcmp(text, "datagram", 8) == 0 && length == sizeof(peer) && peer.sin_port != 0);
    iovec outgoing[] = {{const_cast<char*>("hello"), 5}, {const_cast<char*>("!"), 1}};
    msghdr message{};
    message.msg_name = &peer;
    message.msg_namelen = sizeof(peer);
    message.msg_iov = outgoing;
    message.msg_iovlen = 2;
    assert(sendmsg(server, &message, 0) == 6);
    sockaddr_in sender{};
    iovec incoming[] = {{text, sizeof(text)}};
    message = {};
    message.msg_name = &sender;
    message.msg_namelen = sizeof(sender);
    message.msg_iov = incoming;
    message.msg_iovlen = 1;
    assert(recvmsg(client, &message, 0) == 6 && memcmp(text, "hello!", 6) == 0);
    assert(sender.sin_port == address.sin_port && message.msg_namelen == sizeof(sender));
    assert(close(server) == 0 && close(client) == 0);

    server = socket(AF_UNIX, SOCK_DGRAM, 0);
    client = socket(AF_UNIX, SOCK_DGRAM, 0);
    assert(server >= 0 && client >= 0);
    sockaddr_un local{};
    local.sun_family = AF_UNIX;
    int used = snprintf(local.sun_path + 1, sizeof(local.sun_path) - 1, "pizdos-e2e-%ld", long(getpid()));
    socklen_t localLength = offsetof(sockaddr_un, sun_path) + 1 + used;
    assert(bind(server, reinterpret_cast<sockaddr*>(&local), localLength) == 0);
    sockaddr_un named{};
    length = sizeof(named);
    assert(getsockname(server, reinterpret_cast<sockaddr*>(&named), &length) == 0 && length == localLength);
    assert(memcmp(&named, &local, localLength) == 0);
    assert(sendto(client, "unix", 4, 0, reinterpret_cast<sockaddr*>(&local), localLength) == 4);
    assert(recv(server, text, sizeof(text), 0) == 4 && memcmp(text, "unix", 4) == 0);
    assert(close(server) == 0 && close(client) == 0);
#ifdef __wasm__
    server = socket(AF_UNIX, SOCK_DGRAM, 0);
    client = socket(AF_UNIX, SOCK_DGRAM, 0);
    assert(server >= 0 && client >= 0);
    local = {};
    local.sun_family = AF_UNIX;
    strcpy(local.sun_path, "/tmp/message.sock");
    assert(bind(server, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == 0);
    length = sizeof(named);
    assert(getsockname(server, reinterpret_cast<sockaddr*>(&named), &length) == 0);
    assert(named.sun_family == AF_UNIX && strcmp(named.sun_path, local.sun_path) == 0);
    assert(sendto(client, "path", 4, 0, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == 4);
    assert(recv(server, text, sizeof(text), 0) == 4 && memcmp(text, "path", 4) == 0);
    assert(close(server) == 0 && close(client) == 0 && unlink(local.sun_path) == 0);
#endif
    puts("network sockets: ok");
}
