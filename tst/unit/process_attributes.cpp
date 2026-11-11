//@ compile-flags: -g -D_GNU_SOURCE
//@ stdout: "process attributes: ok\n"

#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

int main() {
    assert(prctl(PR_SET_DUMPABLE, 0) == 0);
    assert(prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0);
    assert(prctl(PR_SET_PDEATHSIG, SIGTERM) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        assert(prctl(PR_GET_DUMPABLE) == 0);
        assert(prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) == 1);
        int signal = -1;
        assert(prctl(PR_GET_PDEATHSIG, &signal) == 0 && signal == 0);
        assert(prctl(PR_SET_DUMPABLE, 1) == 0 && prctl(PR_GET_DUMPABLE) == 1);
        _exit(17);
    }
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 17);
    assert(prctl(PR_GET_DUMPABLE) == 0 && prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) == 1);
    int signal = 0;
    assert(prctl(PR_GET_PDEATHSIG, &signal) == 0 && signal == SIGTERM);
    assert(prctl(PR_SET_PDEATHSIG, 0) == 0 && prctl(PR_SET_DUMPABLE, 1) == 0);
    puts("process attributes: ok");
}
