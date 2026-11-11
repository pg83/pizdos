//@ compile-flags: -g -D_GNU_SOURCE -fno-optimize-sibling-calls
//@ stdout: "nonlocal jumps: ok\n"

#include <assert.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

static jmp_buf repeated, outside, forkJump;
static sigjmp_buf savedMask, signalJump;
static volatile int trips;
static volatile sig_atomic_t caught;

static void replaceMask(int sig) {
    sigset_t mask;
    assert(sigemptyset(&mask) == 0);
    assert(sigaddset(&mask, sig) == 0);
    assert(sigprocmask(SIG_SETMASK, &mask, nullptr) == 0);
}

static void checkMask(int present, int absent) {
    sigset_t mask;
    assert(sigprocmask(SIG_SETMASK, nullptr, &mask) == 0);
    assert(sigismember(&mask, present) == 1);
    assert(sigismember(&mask, absent) == 0);
}

__attribute__((noinline)) static int leaveNested(int depth) {
    jmp_buf inside;
    volatile unsigned char scratch[127];
    scratch[0] = static_cast<unsigned char>(depth);
    int value = setjmp(inside);
    assert(value == 0);
    if (depth) {
        leaveNested(depth - 1);
    }
    longjmp(outside, 42);
    return scratch[0];
}

static void signalAction(int sig) {
    assert(sig == SIGUSR1);
    caught = caught + 1;
    siglongjmp(signalJump, 37);
}

__attribute__((noinline)) static int deepFork(int depth) {
    // Keep native frames live above fork, beyond the initial capture buffer.
    volatile uint64_t marker = uint64_t(depth) * 65537;
    if (depth) {
        int value = deepFork(depth - 1);
        assert(marker == uint64_t(depth) * 65537);
        return value;
    }
    pid_t child = fork();
    assert(child >= 0);
    return child == 0;
}

int main() {
    int value = setjmp(repeated);
    assert(value == trips);
    if (trips < 12) {
        trips = trips + 1;
        longjmp(repeated, trips);
    }
    assert(value == 12);
    for (int k = 0; k != 4; ++k) {
        if (_setjmp(repeated) == 0) {
            _longjmp(repeated, 0);
        }
    }
    value = setjmp(outside);
    if (value == 0) {
        leaveNested(8);
    }
    assert(value == 42);

    sigset_t original;
    assert(sigprocmask(SIG_SETMASK, nullptr, &original) == 0);
    replaceMask(SIGUSR1);
    value = sigsetjmp(savedMask, 1);
    if (value == 0) {
        replaceMask(SIGUSR2);
        siglongjmp(savedMask, 0);
    }
    assert(value == 1);
    checkMask(SIGUSR1, SIGUSR2);
    value = sigsetjmp(savedMask, 0);
    if (value == 0) {
        replaceMask(SIGUSR2);
        siglongjmp(savedMask, -7);
    }
    assert(value == -7);
    checkMask(SIGUSR2, SIGUSR1);
    assert(sigprocmask(SIG_SETMASK, &original, nullptr) == 0);

    sigset_t one;
    assert(sigemptyset(&one) == 0 && sigaddset(&one, SIGUSR1) == 0);
    assert(sigprocmask(SIG_UNBLOCK, &one, nullptr) == 0);
    struct sigaction action{}, old{};
    action.sa_handler = signalAction;
    assert(sigemptyset(&action.sa_mask) == 0);
    assert(sigaction(SIGUSR1, &action, &old) == 0);
    value = sigsetjmp(signalJump, 1);
    if (value == 0) {
        assert(raise(SIGUSR1) == 0);
        assert(false);
    }
    assert(value == 37 && caught == 1);
    sigset_t after;
    assert(sigprocmask(SIG_SETMASK, nullptr, &after) == 0);
    assert(sigismember(&after, SIGUSR1) == 0);
    assert(sigaction(SIGUSR1, &old, nullptr) == 0);
    assert(sigprocmask(SIG_SETMASK, &original, nullptr) == 0);

    value = setjmp(forkJump);
    if (value == 0) {
        if (deepFork(640)) {
            longjmp(forkJump, 23);
        }
    } else {
        assert(value == 23);
        _exit(19);
    }
    int status;
    assert(waitpid(-1, &status, 0) > 0);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 19);
    puts("nonlocal jumps: ok");
}
