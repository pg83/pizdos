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

static volatile sig_atomic_t interrupts;

static void interrupted(int) {
    interrupts = interrupts + 1;
    if (interrupts > 100) {
        _exit(89);
    }
}

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

    // A repeating signal also covers delivery just before the blocking call.
    // Without SA_RESTART it interrupts an unlimited backlog wait.
    struct sigaction action{}, saved{};
    action.sa_handler = interrupted;
    assert(sigemptyset(&action.sa_mask) == 0 && sigaction(SIGALRM, &action, &saved) == 0);
    timeout = {0, 0};
    assert(setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0);
    ualarm(20000, 20000);
    assert(connect(client, reinterpret_cast<sockaddr*>(&address), length) == -1 && errno == EINTR);
    ualarm(0, 0);
    assert(interrupts > 0);

    // The same socket can connect once accept frees a place in the queue.
    // SA_RESTART keeps this wait running while its handler handles signals.
    action.sa_flags = SA_RESTART;
    assert(sigaction(SIGALRM, &action, nullptr) == 0);
    interrupts = 0;
    timeout = {0, 0};
    assert(setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        assert(signal(SIGALRM, SIG_DFL) != SIG_ERR);
        alarm(2);
        assert(close(client) == 0 && close(queued) == 0 && usleep(100000) == 0);
        int first = accept(listener, nullptr, nullptr);
        assert(first >= 0 && close(first) == 0);
        int next = accept(listener, nullptr, nullptr);
        char byte;
        assert(next >= 0 && recv(next, &byte, 1, 0) == 1 && byte == 'x');
        assert(send(next, "y", 1, 0) == 1 && close(next) == 0 && close(listener) == 0);
        _exit(17);
    }
    ualarm(20000, 20000);
    assert(connect(client, reinterpret_cast<sockaddr*>(&address), length) == 0);
    ualarm(0, 0);
    assert(interrupts > 0 && sigaction(SIGALRM, &saved, nullptr) == 0);
    assert(send(client, "x", 1, 0) == 1);
    char byte;
    assert(recv(client, &byte, 1, 0) == 1 && byte == 'y');
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 17);
    assert(close(client) == 0 && close(queued) == 0 && close(listener) == 0);
    alarm(0);
    puts("unix connection backlog: ok");
}
