//@ compile-flags: -g -D_GNU_SOURCE
//@ stdout: "tcp connection timeouts: ok\n"

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

int main() {
    alarm(2);
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(listener >= 0 && bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    socklen_t length = sizeof(address);
    assert(getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) == 0);
    assert(listen(listener, 0) == 0);
    int queued = socket(AF_INET, SOCK_STREAM, 0);
    assert(queued >= 0 && connect(queued, reinterpret_cast<sockaddr*>(&address), length) == 0);

    int client = socket(AF_INET, SOCK_STREAM, 0);
    timeval timeout{0, 20000};
    assert(client >= 0 && setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0);
    // The send timeout ends this blocking wait, while the connection is
    // still pending in TCP. It is not TCP's own connection timeout.
    assert(connect(client, reinterpret_cast<sockaddr*>(&address), length) == -1 && errno == EINPROGRESS);
    int error = -1;
    length = sizeof(error);
    assert(getsockopt(client, SOL_SOCKET, SO_ERROR, &error, &length) == 0 && error == 0);
    assert(close(client) == 0);
    int accepted = accept(listener, nullptr, nullptr);
    assert(accepted >= 0 && close(accepted) == 0 && close(queued) == 0 && close(listener) == 0);
    alarm(0);
    puts("tcp connection timeouts: ok");
}
