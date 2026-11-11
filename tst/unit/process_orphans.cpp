//@ host: false
//@ compile-flags: -g -D_GNU_SOURCE
//@ stdout: "process orphans: ok\n"

#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

static void waitFor(pid_t child, int code) {
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == code);
}

static void checkAdoption(bool zombie) {
    int reports[2], ready[2], release[2];
    assert(pipe(reports) == 0 && pipe(ready) == 0 && pipe(release) == 0);
    pid_t original = getpid();
    pid_t parent = fork();
    assert(parent >= 0);
    if (parent == 0) {
        pid_t intermediate = getpid();
        pid_t child = fork();
        assert(child >= 0);
        if (child == 0) {
            if (zombie) {
                _exit(23);
            }
            sigset_t set;
            assert(sigemptyset(&set) == 0 && sigaddset(&set, SIGUSR1) == 0);
            assert(sigprocmask(SIG_BLOCK, &set, nullptr) == 0);
            assert(prctl(PR_SET_PDEATHSIG, SIGUSR1) == 0);
            assert(getppid() == intermediate);
            assert(write(ready[1], "r", 1) == 1);
            assert(sigwaitinfo(&set, nullptr) == SIGUSR1);
            assert(getppid() == original);
            assert(write(reports[1], "s", 1) == 1);
            _exit(19);
        }
        if (zombie) {
            siginfo_t info{};
            assert(waitid(P_PID, child, &info, WEXITED | WNOWAIT) == 0);
            assert(info.si_pid == child && info.si_code == CLD_EXITED && info.si_status == 23);
        } else {
            char byte;
            assert(read(ready[0], &byte, 1) == 1 && byte == 'r');
        }
        assert(write(reports[1], &child, sizeof(child)) == sizeof(child));
        char byte;
        assert(read(release[0], &byte, 1) == 1 && byte == 'x');
        _exit(17);
    }
    pid_t child;
    assert(read(reports[0], &child, sizeof(child)) == sizeof(child));
    assert(child != parent && child != original);
    assert(write(release[1], "x", 1) == 1);
    waitFor(parent, 17);
    if (!zombie) {
        char byte;
        assert(read(reports[0], &byte, 1) == 1 && byte == 's');
    }
    waitFor(child, zombie ? 23 : 19);
    for (int fd : reports) {
        assert(close(fd) == 0);
    }
    for (int fd : ready) {
        assert(close(fd) == 0);
    }
    for (int fd : release) {
        assert(close(fd) == 0);
    }
}

int main() {
    alarm(10);
    checkAdoption(false);
    checkAdoption(true);
    alarm(0);
    puts("process orphans: ok");
}
