//@ compile-flags: -g
//@ stdout: "signal wait interrupt: ok\n"

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <sys/time.h>

static volatile sig_atomic_t caught;

static void alarmed(int signal) {
    caught = signal;
}

int main() {
    struct sigaction action{};
    action.sa_handler = alarmed;
    assert(sigemptyset(&action.sa_mask) == 0 && sigaction(SIGALRM, &action, nullptr) == 0);
    sigset_t waited, previous;
    assert(sigemptyset(&waited) == 0 && sigaddset(&waited, SIGUSR1) == 0);
    assert(sigprocmask(SIG_BLOCK, &waited, &previous) == 0);
    itimerval timer{};
    timer.it_value.tv_usec = timer.it_interval.tv_usec = 10000;
    assert(setitimer(ITIMER_REAL, &timer, nullptr) == 0);
    timespec limit{1, 0};
    siginfo_t info{};
    assert(sigtimedwait(&waited, &info, &limit) == -1 && errno == EINTR);
    assert(caught == SIGALRM);
    timer = {};
    assert(setitimer(ITIMER_REAL, &timer, nullptr) == 0);
    assert(sigprocmask(SIG_SETMASK, &previous, nullptr) == 0);
    puts("signal wait interrupt: ok");
}
