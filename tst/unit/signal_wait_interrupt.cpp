//@ compile-flags: -g -D_GNU_SOURCE
//@ stdout: "signal wait interrupt: ok\n"

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <sys/time.h>
#include <unistd.h>

#ifndef __wasm__
#include <sys/syscall.h>
#endif

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
#ifdef __wasm__
    int result = sigtimedwait(&waited, &info, &limit);
#else
    // musl retries EINTR in its wrapper; use Linux's signal-wait contract
    // as the reference for the kernel ABI exposed by the guest C library.
    long result = syscall(SYS_rt_sigtimedwait, &waited, &info, &limit, 8);
#endif
    assert(result == -1 && errno == EINTR);
    assert(caught == SIGALRM);
    timer = {};
    assert(setitimer(ITIMER_REAL, &timer, nullptr) == 0);
    assert(sigprocmask(SIG_SETMASK, &previous, nullptr) == 0);
    puts("signal wait interrupt: ok");
}
