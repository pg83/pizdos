//@ compile-flags: -g -D_GNU_SOURCE
//@ stdout: "descriptor polling: ok\n"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t signals;

static void alarmed(int) {
    signals = signals + 1;
}

int main() {
    timespec zero{};
    timeval instant{};
    assert(poll(nullptr, 0, 0) == 0 && ppoll(nullptr, 0, &zero, nullptr) == 0);
    assert(select(0, nullptr, nullptr, nullptr, &instant) == 0);
    assert(pselect(0, nullptr, nullptr, nullptr, &zero, nullptr) == 0);
    int ends[2];
    assert(pipe2(ends, O_NONBLOCK | O_CLOEXEC) == 0);
    pollfd descriptors[] = {{ends[0], POLLIN, 0}, {ends[1], POLLOUT, 0}, {-1, POLLIN, 123}};
    assert(poll(descriptors, 3, 0) == 1 && descriptors[0].revents == 0 && descriptors[2].revents == 0);
    assert(descriptors[1].revents & POLLOUT);
    assert(write(ends[1], "x", 1) == 1);
    assert(poll(descriptors, 3, 0) == 2 && descriptors[0].revents & POLLIN);
    int available = 0;
    assert(ioctl(ends[0], FIONREAD, &available) == 0 && available == 1);
    int block = 0;
    assert(ioctl(ends[0], FIONBIO, &block) == 0 && (fcntl(ends[0], F_GETFL) & O_NONBLOCK) == 0);
    block = 1;
    assert(ioctl(ends[0], FIONBIO, &block) == 0 && fcntl(ends[0], F_GETFL) & O_NONBLOCK);
    sigset_t masked{}, empty{}, saved{}, observed{};
    assert(sigemptyset(&masked) == 0 && sigemptyset(&empty) == 0);
    assert(sigaddset(&masked, SIGUSR2) == 0 && sigprocmask(SIG_BLOCK, &masked, &saved) == 0);
    assert(ppoll(descriptors, 3, &zero, &empty) == 2);
    fd_set readSet, writeSet, errorSet;
    FD_ZERO(&readSet);
    FD_ZERO(&writeSet);
    FD_ZERO(&errorSet);
    FD_SET(ends[0], &readSet);
    FD_SET(ends[1], &writeSet);
    FD_SET(ends[0], &errorSet);
    assert(pselect(ends[1] + 1, &readSet, &writeSet, &errorSet, &zero, &empty) == 2);
    assert(FD_ISSET(ends[0], &readSet) && FD_ISSET(ends[1], &writeSet) && !FD_ISSET(ends[0], &errorSet));
    assert(sigprocmask(SIG_SETMASK, nullptr, &observed) == 0 && sigismember(&observed, SIGUSR2) == 1);
    timespec invalid{-1, 0};
    assert(pselect(0, nullptr, nullptr, nullptr, &invalid, nullptr) == -1 && errno == EINVAL);
    invalid = {0, 1000000000};
    assert(ppoll(nullptr, 0, &invalid, nullptr) == -1 && errno == EINVAL);
    instant = {-1, 0};
    assert(select(0, nullptr, nullptr, nullptr, &instant) == -1 && errno == EINVAL);
    assert(select(-1, nullptr, nullptr, nullptr, nullptr) == -1 && errno == EINVAL);
    char byte;
    assert(read(ends[0], &byte, 1) == 1 && byte == 'x');
    assert(close(ends[1]) == 0);
    assert(poll(descriptors, 1, 0) == 1 && descriptors[0].revents & POLLHUP);
    assert(read(ends[0], &byte, 1) == 0);
    assert(close(ends[0]) == 0);
    assert(poll(descriptors, 1, 0) == 1 && descriptors[0].revents & POLLNVAL);
    FD_ZERO(&readSet);
    FD_SET(ends[0], &readSet);
    assert(pselect(ends[0] + 1, &readSet, nullptr, nullptr, &zero, nullptr) == -1 && errno == EBADF);

    struct sigaction action{};
    action.sa_handler = alarmed;
    assert(sigemptyset(&action.sa_mask) == 0 && sigaction(SIGALRM, &action, nullptr) == 0);
    assert(ualarm(10000, 10000) == 0);
    timespec delay{1, 0};
    assert(pselect(0, nullptr, nullptr, nullptr, &delay, &empty) == -1 && errno == EINTR);
    assert(ppoll(nullptr, 0, &delay, &empty) == -1 && errno == EINTR);
    ualarm(0, 0);
    assert(signals >= 2 && delay.tv_sec == 1 && delay.tv_nsec == 0);
    assert(sigprocmask(SIG_SETMASK, nullptr, &observed) == 0 && sigismember(&observed, SIGUSR2) == 1);
    assert(sigprocmask(SIG_SETMASK, &saved, nullptr) == 0);
    puts("descriptor polling: ok");
}
