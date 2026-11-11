//@ compile-flags: -g -D_GNU_SOURCE
//@ stdout: "timer rearming: ok\n"

#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <sys/time.h>
#include <time.h>

int main() {
    assert(signal(SIGALRM, SIG_IGN) != SIG_ERR);
    for (unsigned i = 0; i < 10000; ++i) {
        itimerval timer{};
        timer.it_value.tv_usec = 1;
        assert(setitimer(ITIMER_REAL, &timer, nullptr) == 0);
        timer = {};
        assert(setitimer(ITIMER_REAL, &timer, nullptr) == 0);
    }
    itimerval timer{}, observed{};
    timer.it_value.tv_usec = timer.it_interval.tv_usec = 1;
    assert(setitimer(ITIMER_REAL, &timer, nullptr) == 0);
    timespec delay{0, 20000000};
    assert(nanosleep(&delay, nullptr) == 0);
    assert(getitimer(ITIMER_REAL, &observed) == 0);
    assert(observed.it_interval.tv_sec == 0 && observed.it_interval.tv_usec == 1);
    timer = {};
    assert(setitimer(ITIMER_REAL, &timer, nullptr) == 0);
    assert(getitimer(ITIMER_REAL, &observed) == 0);
    assert(!timerisset(&observed.it_value) && !timerisset(&observed.it_interval));
    puts("timer rearming: ok");
}
