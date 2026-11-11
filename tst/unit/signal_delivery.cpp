//@ compile-flags: -g -D_GNU_SOURCE
//@ stdout: "signal delivery: ok\n"

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t handled;
static unsigned char* alternate;

static void action(int sig, siginfo_t* info, void*) {
    int savedErrno = errno;
    volatile unsigned char local;
    assert(sig == SIGUSR1 && info && info->si_signo == sig && info->si_pid == getpid());
    assert(uintptr_t(&local) >= uintptr_t(alternate) && uintptr_t(&local) < uintptr_t(alternate) + 65536);
    stack_t old{};
    assert(sigaltstack(nullptr, &old) == 0 && (old.ss_flags & SS_ONSTACK));
    stack_t disabled{};
    disabled.ss_flags = SS_DISABLE;
    assert(sigaltstack(&disabled, nullptr) == -1 && errno == EPERM);
    sigset_t mask;
    assert(sigprocmask(SIG_SETMASK, nullptr, &mask) == 0);
    assert(sigismember(&mask, SIGUSR1) && sigismember(&mask, SIGUSR2));
    handled = handled + 1;
    errno = savedErrno;
}

int main() {
    sigset_t a, b, united, intersection, saved;
    assert(sigemptyset(&a) == 0 && sigisemptyset(&a));
    assert(sigaddset(&a, SIGUSR1) == 0 && sigaddset(&a, SIGUSR2) == 0);
    assert(sigaddset(&a, 0) == -1 && errno == EINVAL);
    assert(sigaddset(&a, NSIG) == -1 && errno == EINVAL);
    assert(sigfillset(&b) == 0 && sigdelset(&b, SIGUSR2) == 0);
    assert(sigdelset(&b, 0) == -1 && errno == EINVAL);
    assert(sigorset(&united, &a, &b) == 0 && sigismember(&united, SIGUSR2));
    assert(sigandset(&intersection, &a, &b) == 0);
    assert(sigismember(&intersection, SIGUSR1) && !sigismember(&intersection, SIGUSR2));
    assert(!sigisemptyset(&intersection));
    assert(sigprocmask(SIG_BLOCK, &a, &saved) == 0);
    assert(sigprocmask(99, &a, nullptr) == -1 && errno == EINVAL);
    assert(kill(getpid(), SIGUSR1) == 0 && kill(getpid(), SIGUSR2) == 0);
    sigset_t pending;
    assert(sigpending(&pending) == 0 && sigismember(&pending, SIGUSR1) && sigismember(&pending, SIGUSR2));
    siginfo_t info{};
    assert(sigwaitinfo(&a, &info) == SIGUSR1 && info.si_signo == SIGUSR1);
    timespec zero{}, invalid{0, 1000000000};
    assert(sigtimedwait(&a, &info, &zero) == SIGUSR2 && info.si_signo == SIGUSR2);
    assert(sigtimedwait(&a, &info, &zero) == -1 && errno == EAGAIN);
    assert(sigtimedwait(&a, &info, &invalid) == -1 && errno == EINVAL);
    timespec shortWait{0, 1000000};
    assert(sigtimedwait(&a, &info, &shortWait) == -1 && errno == EAGAIN);
    assert(kill(getpid(), SIGUSR1) == 0 && sigwaitinfo(&a, nullptr) == SIGUSR1);
    union sigval value{};
    value.sival_int = 37;
    assert(sigqueue(getpid(), SIGUSR2, value) == 0);
    int received;
    assert(sigwait(&a, &received) == 0 && received == SIGUSR2);
    assert(sigprocmask(SIG_SETMASK, &saved, nullptr) == 0);

    assert(signal(0, SIG_IGN) == SIG_ERR && errno == EINVAL);
    assert(signal(SIGKILL, SIG_IGN) == SIG_ERR && errno == EINVAL);
    struct sigaction install{}, previous{}, current{};
    assert(sigemptyset(&install.sa_mask) == 0);
    install.sa_handler = SIG_IGN;
    assert(sigaction(SIGSTOP, &install, nullptr) == -1 && errno == EINVAL);
    assert(sigaction(0, nullptr, &current) == -1 && errno == EINVAL);
    assert(kill(getpid(), NSIG) == -1 && errno == EINVAL);
#ifdef __wasm__
    assert(raise(0) == -1 && errno == EINVAL);
    assert(sigqueue(0, 0, value) == -1 && errno == EINVAL);
    assert(killpg(1, 0) == -1 && errno == EINVAL);
#endif
    assert(kill(0x7fffffff, 0) == -1 && errno == ESRCH);
    assert(kill(0, 0) == 0);

    stack_t old{}, stack{};
    assert(sigaltstack(nullptr, &old) == 0 && (old.ss_flags & SS_DISABLE));
    alternate = static_cast<unsigned char*>(malloc(65536));
    assert(alternate);
    stack.ss_sp = alternate;
    stack.ss_size = 65536;
    stack.ss_flags = 0x20;
    assert(sigaltstack(&stack, nullptr) == -1 && errno == EINVAL);
    stack.ss_flags = 0;
    stack.ss_size = 1;
    assert(sigaltstack(&stack, nullptr) == -1 && errno == ENOMEM);
    stack.ss_size = 65536;
    assert(sigaltstack(&stack, nullptr) == 0);
    install.sa_sigaction = action;
    install.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
    assert(sigaddset(&install.sa_mask, SIGUSR2) == 0);
    assert(sigaction(SIGUSR1, &install, &previous) == 0);
    assert(siginterrupt(SIGUSR1, 1) == 0);
    assert(sigaction(SIGUSR1, nullptr, &current) == 0 && !(current.sa_flags & SA_RESTART));
    assert(siginterrupt(SIGUSR1, 0) == 0);
    assert(sigaction(SIGUSR1, nullptr, &current) == 0 && (current.sa_flags & SA_RESTART));
    assert(siginterrupt(0, 1) == -1 && errno == EINVAL);
    errno = E2BIG;
    assert(kill(getpid(), SIGUSR1) == 0 && handled == 1 && errno == E2BIG);
    assert(kill(getpid(), SIGUSR1) == 0 && handled == 2 && errno == E2BIG);
    assert(sigaction(SIGUSR1, &previous, nullptr) == 0);
    stack.ss_flags = SS_DISABLE;
    assert(sigaltstack(&stack, nullptr) == 0);
    free(alternate);

    int ready[2];
    assert(pipe(ready) == 0);
    assert(sigprocmask(SIG_BLOCK, &a, &saved) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        assert(setpgid(0, 0) == 0);
        assert(write(ready[1], "r", 1) == 1);
        assert(sigwait(&a, &received) == 0 && received == SIGUSR1);
        _exit(17);
    }
    char byte;
    assert(read(ready[0], &byte, 1) == 1 && byte == 'r');
    assert(kill(-1, 0) == 0);
    assert(killpg(child, SIGUSR1) == 0);
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 17);
    assert(sigprocmask(SIG_SETMASK, &saved, nullptr) == 0);
    assert(close(ready[0]) == 0 && close(ready[1]) == 0);
    puts("signal delivery: ok");
}
