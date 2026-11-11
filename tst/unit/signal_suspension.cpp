//@ compile-flags: -g -D_GNU_SOURCE
//@ stdout: "signal suspension: ok\n"

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <sys/time.h>
#include <unistd.h>

static volatile sig_atomic_t caught;

static void alarmed(int signal) {
    assert(signal == SIGALRM);
    caught = caught + 1;
}

int main() {
    struct sigaction action{};
    action.sa_handler = alarmed;
    assert(sigemptyset(&action.sa_mask) == 0 && sigaction(SIGALRM, &action, nullptr) == 0);
    sigset_t blocked, previous, temporary, current;
    assert(sigemptyset(&blocked) == 0 && sigaddset(&blocked, SIGALRM) == 0);
    assert(sigprocmask(SIG_BLOCK, &blocked, &previous) == 0);
    assert(sigemptyset(&temporary) == 0);
    itimerval timer{};
    timer.it_value.tv_usec = 10000;
    assert(setitimer(ITIMER_REAL, &timer, nullptr) == 0);
    assert(sigsuspend(&temporary) == -1 && errno == EINTR && caught == 1);
    assert(sigprocmask(SIG_SETMASK, nullptr, &current) == 0 && sigismember(&current, SIGALRM) == 1);
    assert(raise(SIGALRM) == 0 && caught == 1);
    assert(sigpending(&current) == 0 && sigismember(&current, SIGALRM) == 1);
    assert(sigsuspend(&temporary) == -1 && errno == EINTR && caught == 2);
    assert(sigpending(&current) == 0 && sigismember(&current, SIGALRM) == 0);
    assert(sigprocmask(SIG_SETMASK, &previous, nullptr) == 0);
    puts("signal suspension: ok");
}
