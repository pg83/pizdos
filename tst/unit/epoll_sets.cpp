//@ compile-flags: -g -D_GNU_SOURCE
//@ stdout: "epoll sets: ok\n"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

// An epoll set reports its ready members level-triggered, once a change
// with EPOLLET, once until re-armed with EPOLLONESHOT; it is itself a
// file one can poll or put in another set; a regular file cannot join.

static volatile sig_atomic_t alarms;

static void alarmed(int) {
    alarms = alarms + 1;
}

static int waitOne(int ep, epoll_event* event, int timeout) {
    return epoll_wait(ep, event, 1, timeout);
}

static long elapsedMs(const timespec& since) {
    timespec now;
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return (now.tv_sec - since.tv_sec) * 1000 + (now.tv_nsec - since.tv_nsec) / 1000000;
}

int main() {
    assert(epoll_create(0) == -1 && errno == EINVAL);
    assert(epoll_create1(1) == -1 && errno == EINVAL);
    int ep = epoll_create1(EPOLL_CLOEXEC);
    assert(ep >= 0 && (fcntl(ep, F_GETFD) & FD_CLOEXEC) && (fcntl(ep, F_GETFL) & O_ACCMODE) == O_RDWR);
    char byte;
    assert(read(ep, &byte, 1) == -1 && errno == EINVAL);
    epoll_event event{};
    assert(epoll_wait(ep, &event, 0, 0) == -1 && errno == EINVAL);
    assert(epoll_wait(ep, &event, -1, 0) == -1 && errno == EINVAL);
    assert(epoll_wait(999, &event, 1, 0) == -1 && errno == EBADF);
    assert(waitOne(ep, &event, 0) == 0);

    int ends[2];
    assert(pipe2(ends, O_NONBLOCK) == 0);
    assert(epoll_wait(ends[0], &event, 1, 0) == -1 && errno == EINVAL);
    event.events = EPOLLIN;
    event.data.u64 = 0x1122334455667788ull;
    assert(epoll_ctl(ep, EPOLL_CTL_ADD, ep, &event) == -1 && errno == EINVAL);
    assert(epoll_ctl(ep, EPOLL_CTL_ADD, 999, &event) == -1 && errno == EBADF);
    assert(epoll_ctl(ends[0], EPOLL_CTL_ADD, ends[1], &event) == -1 && errno == EINVAL);
    assert(epoll_ctl(ep, EPOLL_CTL_MOD, ends[0], &event) == -1 && errno == ENOENT);
    assert(epoll_ctl(ep, EPOLL_CTL_DEL, ends[0], nullptr) == -1 && errno == ENOENT);
    assert(epoll_ctl(ep, EPOLL_CTL_ADD, ends[0], &event) == 0);
    assert(epoll_ctl(ep, EPOLL_CTL_ADD, ends[0], &event) == -1 && errno == EEXIST);
    assert(epoll_ctl(ep, 7, ends[0], &event) == -1 && errno == EINVAL);
    int file = open("epoll-member", O_CREAT | O_RDWR | O_TRUNC, 0600);
    assert(file >= 0);
    assert(epoll_ctl(ep, EPOLL_CTL_ADD, file, &event) == -1 && errno == EPERM);
    assert(close(file) == 0 && unlink("epoll-member") == 0);

    // Level-triggered: ready while data is there, with the data word back.
    assert(waitOne(ep, &event, 0) == 0);
    pollfd self = {ep, POLLIN, 0};
    assert(poll(&self, 1, 0) == 0);
    assert(write(ends[1], "ab", 2) == 2);
    event = {};
    assert(waitOne(ep, &event, -1) == 1 && event.events == EPOLLIN && event.data.u64 == 0x1122334455667788ull);
    assert(waitOne(ep, &event, 0) == 1);
    assert(poll(&self, 1, 0) == 1 && self.revents == POLLIN);
    assert(read(ends[0], &byte, 1) == 1 && byte == 'a');
    assert(waitOne(ep, &event, 0) == 1);
    assert(read(ends[0], &byte, 1) == 1 && byte == 'b');
    assert(waitOne(ep, &event, 0) == 0);

    // Edge-triggered: once a change, whatever is left.
    event.events = EPOLLIN | EPOLLET;
    event.data.fd = ends[0];
    assert(epoll_ctl(ep, EPOLL_CTL_MOD, ends[0], &event) == 0);
    assert(write(ends[1], "cd", 2) == 2);
    assert(waitOne(ep, &event, 0) == 1 && event.data.fd == ends[0]);
    assert(waitOne(ep, &event, 0) == 0);
    assert(write(ends[1], "e", 1) == 1);
    assert(waitOne(ep, &event, 0) == 1);
    assert(waitOne(ep, &event, 0) == 0);
    assert(read(ends[0], &byte, 1) == 1 && read(ends[0], &byte, 1) == 1 && read(ends[0], &byte, 1) == 1);

    // One-shot: once, until re-armed; re-arming checks readiness anew.
    event.events = EPOLLIN | EPOLLONESHOT;
    assert(epoll_ctl(ep, EPOLL_CTL_MOD, ends[0], &event) == 0);
    assert(write(ends[1], "f", 1) == 1);
    assert(waitOne(ep, &event, 0) == 1);
    assert(waitOne(ep, &event, 0) == 0);
    assert(write(ends[1], "g", 1) == 1);
    assert(waitOne(ep, &event, 0) == 0);
    assert(epoll_ctl(ep, EPOLL_CTL_MOD, ends[0], &event) == 0);
    assert(waitOne(ep, &event, 0) == 1);
    assert(read(ends[0], &byte, 1) == 1 && read(ends[0], &byte, 1) == 1);
    assert(waitOne(ep, &event, 0) == 0);

    // Several ready members come out one call after another, writers by
    // room, a pair's end by its peer's close.
    int more[2];
    assert(pipe2(more, O_NONBLOCK) == 0);
    event.events = EPOLLIN;
    event.data.fd = ends[0];
    assert(epoll_ctl(ep, EPOLL_CTL_MOD, ends[0], &event) == 0);
    event.data.fd = more[0];
    assert(epoll_ctl(ep, EPOLL_CTL_ADD, more[0], &event) == 0);
    event.events = EPOLLOUT;
    event.data.fd = more[1];
    assert(epoll_ctl(ep, EPOLL_CTL_ADD, more[1], &event) == 0);
    assert(waitOne(ep, &event, 0) == 1 && event.data.fd == more[1] && event.events == EPOLLOUT);
    assert(write(ends[1], "h", 1) == 1 && write(more[1], "i", 1) == 1);
    epoll_event three[3] = {};
    assert(epoll_wait(ep, three, 3, 0) == 3);
    int seen = 0;
    for (const epoll_event& e : three) {
        seen |= 1 << (e.data.fd == ends[0] ? 0 : e.data.fd == more[0] ? 1 : e.data.fd == more[1] ? 2 : 3);
    }
    assert(seen == 7);
    assert(epoll_ctl(ep, EPOLL_CTL_DEL, more[1], nullptr) == 0);
    assert(waitOne(ep, &event, 0) == 1);
    int first = event.data.fd;
    assert(waitOne(ep, &event, 0) == 1 && event.data.fd != first);
    assert(read(ends[0], &byte, 1) == 1 && read(more[0], &byte, 1) == 1);
    assert(epoll_ctl(ep, EPOLL_CTL_DEL, more[0], nullptr) == 0);
    assert(close(more[0]) == 0 && close(more[1]) == 0);
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair) == 0);
    event.events = EPOLLIN;
    event.data.fd = pair[0];
    assert(epoll_ctl(ep, EPOLL_CTL_ADD, pair[0], &event) == 0);
    assert(waitOne(ep, &event, 0) == 0);
    assert(close(pair[1]) == 0);
    assert(waitOne(ep, &event, 0) == 1 && event.data.fd == pair[0] && (event.events & (EPOLLIN | EPOLLHUP)) == (EPOLLIN | EPOLLHUP));
    assert(epoll_ctl(ep, EPOLL_CTL_DEL, pair[0], nullptr) == 0);
    assert(close(pair[0]) == 0);

    // A set in a set: ready when a member of the inner one is; no loops.
    int outer = epoll_create1(0);
    assert(outer >= 0);
    event.events = EPOLLIN;
    event.data.fd = ep;
    assert(epoll_ctl(outer, EPOLL_CTL_ADD, ep, &event) == 0);
    event.data.fd = outer;
    assert(epoll_ctl(ep, EPOLL_CTL_ADD, outer, &event) == -1 && errno == ELOOP);
    assert(waitOne(outer, &event, 0) == 0);
    assert(write(ends[1], "j", 1) == 1);
    assert(waitOne(outer, &event, 0) == 1 && event.data.fd == ep && event.events == EPOLLIN);
    assert(read(ends[0], &byte, 1) == 1);
    assert(waitOne(outer, &event, 0) == 0);
    assert(close(outer) == 0);

    // A timeout passes; a handled signal interrupts, restarting or not.
    timespec start;
    assert(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
    assert(waitOne(ep, &event, 30) == 0 && elapsedMs(start) >= 25);
    struct sigaction action = {};
    action.sa_handler = alarmed;
    action.sa_flags = SA_RESTART;
    assert(sigaction(SIGALRM, &action, nullptr) == 0);
    itimerval timer = {{0, 0}, {0, 20000}};
    assert(setitimer(ITIMER_REAL, &timer, nullptr) == 0);
    assert(waitOne(ep, &event, -1) == -1 && errno == EINTR && alarms == 1);
    sigset_t blocked, saved;
    assert(sigemptyset(&blocked) == 0 && sigaddset(&blocked, SIGALRM) == 0);
    assert(sigprocmask(SIG_BLOCK, &blocked, &saved) == 0);
    assert(setitimer(ITIMER_REAL, &timer, nullptr) == 0);
    sigset_t none;
    assert(sigemptyset(&none) == 0);
    assert(epoll_pwait(ep, &event, 1, -1, &none) == -1 && errno == EINTR && alarms == 2);
    assert(sigprocmask(SIG_SETMASK, nullptr, &none) == 0 && sigismember(&none, SIGALRM) == 1);
    assert(sigprocmask(SIG_SETMASK, &saved, nullptr) == 0);

    assert(epoll_ctl(ep, EPOLL_CTL_DEL, ends[0], nullptr) == 0);
    assert(close(ends[0]) == 0 && close(ends[1]) == 0 && close(ep) == 0);
    puts("epoll sets: ok");
    return 0;
}
