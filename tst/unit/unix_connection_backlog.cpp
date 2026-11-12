//@ compile-flags: -g -D_GNU_SOURCE
//@ stdout: "unix connection backlog: ok\n"

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

int main() {
    alarm(2);
    int listener = socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    int used = snprintf(address.sun_path + 1, sizeof(address.sun_path) - 1, "pizdos-backlog-%ld", long(getpid()));
    socklen_t length = offsetof(sockaddr_un, sun_path) + 1 + used;
    assert(listener >= 0 && bind(listener, reinterpret_cast<sockaddr*>(&address), length) == 0);
    assert(listen(listener, 0) == 0);
    int queued = socket(AF_UNIX, SOCK_STREAM, 0);
    assert(queued >= 0 && connect(queued, reinterpret_cast<sockaddr*>(&address), length) == 0);

    int client = socket(AF_UNIX, SOCK_STREAM, 0);
    timeval timeout{0, 20000};
    assert(client >= 0 && setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0);
    assert(connect(client, reinterpret_cast<sockaddr*>(&address), length) == -1 && errno == EAGAIN);
    alarm(0);

    // The same socket can connect once accept frees a place in the queue.
    alarm(2);
    timeout = {1, 0};
    assert(setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        assert(close(client) == 0 && close(queued) == 0 && usleep(20000) == 0);
        int first = accept(listener, nullptr, nullptr);
        assert(first >= 0 && close(first) == 0);
        int next = accept(listener, nullptr, nullptr);
        char byte;
        assert(next >= 0 && recv(next, &byte, 1, 0) == 1 && byte == 'x');
        assert(send(next, "y", 1, 0) == 1 && close(next) == 0 && close(listener) == 0);
        _exit(17);
    }
    assert(connect(client, reinterpret_cast<sockaddr*>(&address), length) == 0);
    assert(send(client, "x", 1, 0) == 1);
    char byte;
    assert(recv(client, &byte, 1, 0) == 1 && byte == 'y');
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 17);
    assert(close(client) == 0 && close(queued) == 0 && close(listener) == 0);
    alarm(0);
    puts("unix connection backlog: ok");
}
