//@ compile-flags: -g -D_GNU_SOURCE -fno-builtin
//@ stdout: "socket errors: ok\n"

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int main() {
    sockaddr_storage address{};
    socklen_t length = sizeof(address);
    assert(getsockname(-1, reinterpret_cast<sockaddr*>(&address), &length) == -1 && errno == EBADF);
    assert(getpeername(-1, reinterpret_cast<sockaddr*>(&address), &length) == -1 && errno == EBADF);
    assert(bind(-1, reinterpret_cast<sockaddr*>(&address), length) == -1 && errno == EBADF);
    msghdr message{};
    assert(sendmsg(-1, &message, 0) == -1 && errno == EBADF);
    assert(recvmsg(-1, &message, 0) == -1 && errno == EBADF);
    int pair[2] = {-1, -1};
    assert(socketpair(AF_INET, SOCK_STREAM, 0, pair) == -1 && errno == EOPNOTSUPP);
    assert(socketpair(AF_UNIX, -1, 0, pair) == -1 && errno == EINVAL);

    int udp = socket(AF_INET, SOCK_DGRAM, 0);
    assert(udp >= 0);
    int value = 0;
    length = sizeof(value);
    assert(getsockopt(udp, SOL_SOCKET, 0x7fffffff, &value, &length) == -1 && errno == ENOPROTOOPT);
    assert(sockatmark(udp) == -1 && errno == ENOTTY);
    unsigned char tooLarge[129]{};
    assert(bind(udp, reinterpret_cast<sockaddr*>(tooLarge), sizeof(tooLarge)) == -1 && errno == EINVAL);
#ifdef __wasm__
    unsigned char option[1025]{};
    assert(setsockopt(udp, SOL_SOCKET, SO_REUSEADDR, option, sizeof(option)) == -1 && errno == EINVAL);
    int local = socket(AF_UNIX, SOCK_DGRAM, 0);
    assert(local >= 0);
    sockaddr_un name{};
    name.sun_family = AF_UNIX;
    memset(name.sun_path + 1, 'x', sizeof(name.sun_path) - 1);
    assert(bind(local, reinterpret_cast<sockaddr*>(&name), sizeof(name)) == -1 && errno == EINVAL);
    memset(name.sun_path, 'x', 100);
    name.sun_path[100] = 0;
    assert(bind(local, reinterpret_cast<sockaddr*>(&name), offsetof(sockaddr_un, sun_path) + 101) == -1 && errno == ENAMETOOLONG);
    assert(close(local) == 0);
#endif
    assert(close(udp) == 0);

    rlimit saved{};
    assert(getrlimit(RLIMIT_NOFILE, &saved) == 0);
    rlimit limited = saved;
    for (rlim_t count = 3; count != 5; ++count) {
        limited.rlim_cur = count;
        assert(setrlimit(RLIMIT_NOFILE, &limited) == 0);
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == -1 && errno == EMFILE);
        if (count == 4) {
            int fd = socket(AF_INET, SOCK_DGRAM, 0);
            assert(fd == 3 && close(fd) == 0);
        }
    }
    assert(setrlimit(RLIMIT_NOFILE, &saved) == 0);

    int listener = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(listener >= 0 && bind(listener, reinterpret_cast<sockaddr*>(&endpoint), sizeof(endpoint)) == 0);
    length = sizeof(endpoint);
    assert(getsockname(listener, reinterpret_cast<sockaddr*>(&endpoint), &length) == 0 && listen(listener, 2) == 0);
    int clients[2];
    for (int& client : clients) {
        client = socket(AF_INET, SOCK_STREAM, 0);
        assert(client >= 0 && connect(client, reinterpret_cast<sockaddr*>(&endpoint), sizeof(endpoint)) == 0);
    }
    limited.rlim_cur = 3;
    assert(setrlimit(RLIMIT_NOFILE, &limited) == 0);
    assert(accept(listener, nullptr, nullptr) == -1 && errno == EMFILE);
    assert(setrlimit(RLIMIT_NOFILE, &saved) == 0);
    int accepted = accept(listener, nullptr, nullptr);
    assert(accepted >= 0 && close(accepted) == 0);
    for (int client : clients) {
        assert(close(client) == 0);
    }
    assert(close(listener) == 0);
    puts("socket errors: ok");
}
