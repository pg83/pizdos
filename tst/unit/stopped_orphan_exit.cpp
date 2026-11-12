//@ compile-flags: -g -D_GNU_SOURCE
//@ stdout: "stopped orphan exit: ok\n"

#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

int main() {
    alarm(10);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        // The first process exits with this child stopped. Its death signal
        // resumes the orphan while the VM tears down the parent's process.
        assert(prctl(PR_SET_PDEATHSIG, SIGCONT) == 0);
        alarm(2);
        assert(raise(SIGSTOP) == 0);
        _exit(0);
    }
    int status;
    assert(waitpid(child, &status, WUNTRACED) == child);
    assert(WIFSTOPPED(status) && WSTOPSIG(status) == SIGSTOP);
    alarm(0);
    puts("stopped orphan exit: ok");
}
