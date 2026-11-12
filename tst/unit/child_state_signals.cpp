//@ compile-flags: -g -D_GNU_SOURCE
//@ stdout: "child state signals: ok\n"

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t alarms;

static void handler(int sig) {
    if (sig == SIGALRM) {
        alarms = alarms + 1;
    }
}

int main() {
    struct sigaction action{}, saved{};
    action.sa_handler = handler;
    assert(sigemptyset(&action.sa_mask) == 0 && sigaction(SIGCHLD, &action, &saved) == 0);
    sigset_t blocked{}, previous{};
    assert(sigemptyset(&blocked) == 0 && sigaddset(&blocked, SIGCHLD) == 0);
    assert(sigprocmask(SIG_BLOCK, &blocked, &previous) == 0);
    int release[2];
    assert(pipe(release) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        assert(raise(SIGSTOP) == 0);
        char byte;
        assert(read(release[0], &byte, 1) == 1 && byte == 'x');
        _exit(17);
    }
    int status;
    assert(waitpid(child, &status, WUNTRACED) == child && WIFSTOPPED(status) && WSTOPSIG(status) == SIGSTOP);
    siginfo_t info{};
    assert(sigwaitinfo(&blocked, &info) == SIGCHLD && info.si_signo == SIGCHLD);
    assert(kill(child, SIGCONT) == 0);
    assert(waitpid(child, &status, WCONTINUED) == child && WIFCONTINUED(status));
    assert(sigwaitinfo(&blocked, &info) == SIGCHLD && info.si_signo == SIGCHLD);
    assert(write(release[1], "x", 1) == 1);
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 17);
    assert(sigwaitinfo(&blocked, &info) == SIGCHLD);
    assert(sigaction(SIGCHLD, &saved, nullptr) == 0 && sigprocmask(SIG_SETMASK, &previous, nullptr) == 0);

    action.sa_flags = SA_RESTART;
    assert(sigaction(SIGALRM, &action, &saved) == 0);
    child = fork();
    assert(child >= 0);
    if (child == 0) {
        assert(usleep(100000) == 0);
        _exit(23);
    }
    ualarm(10000, 0);
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 23);
    assert(alarms == 1 && sigaction(SIGALRM, &saved, nullptr) == 0);
    assert(close(release[0]) == 0 && close(release[1]) == 0);
    puts("child state signals: ok");
}
