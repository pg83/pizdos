//@ compile-flags: -g
//@ stdout: "timer lifecycle: ok\n"

#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

int main() {
    assert(alarm(3600) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        // A fork inherits neither the pending alarm nor its deadline.
        assert(alarm(0) == 0);
        assert(alarm(3600) == 0);
        _exit(0);
    }
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    unsigned left = alarm(0);
    assert(left > 0 && left <= 3600);
    sigset_t pending;
    assert(sigpending(&pending) == 0 && sigismember(&pending, SIGALRM) == 0);
    const int kinds[] = {ITIMER_REAL, ITIMER_VIRTUAL, ITIMER_PROF};
    for (int kind : kinds) {
        itimerval timer{}, old{};
        timer.it_value.tv_sec = 3600;
        assert(setitimer(kind, &timer, &old) == 0 && !timerisset(&old.it_value));
    }
    puts("timer lifecycle: ok");
}
