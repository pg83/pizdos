#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

struct Filter {
    uint16_t code;
    uint8_t yes, no;
    uint32_t value;
};

struct Program {
    unsigned short count;
    struct Filter* instructions;
};

// Return EINTR once at the real syscall boundary, then let the host handle
// every retry. Seccomp limits ptrace stops to the selected syscall; traced
// Go threads inherit the filter and tracing through PTRACE_O_TRACECLONE.
int main(int argc, char** argv) {
    assert(argc >= 5 && strcmp(argv[2], "--") == 0);
    int number = atoi(argv[1]);
    struct stat input;
    assert(fstat(0, &input) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        assert(ptrace(PTRACE_TRACEME, 0, 0, 0) == 0);
        raise(SIGSTOP);
        struct Filter filters[] = {
            {0x20, 0, 0, 4}, {0x15, 1, 0, 0xc000003e},
            {0x06, 0, 0, 0x80000000}, {0x20, 0, 0, 0},
            {0x15, 0, 1, (uint32_t)number}, {0x06, 0, 0, 0x7ff00000},
            {0x06, 0, 0, 0x7fff0000},
        };
        struct Program program = {sizeof(filters) / sizeof(filters[0]), filters};
        assert(prctl(PR_SET_NO_NEW_PRIVS, 1UL, 0UL, 0UL, 0UL) == 0);
        assert(prctl(PR_SET_SECCOMP, 2UL, &program, 0UL, 0UL) == 0);
        execvp(argv[3], argv + 3);
        perror("exec");
        _exit(1);
    }
    int status, injected = 0;
    assert(waitpid(child, &status, 0) == child && WIFSTOPPED(status));
    long options = PTRACE_O_TRACESECCOMP | PTRACE_O_TRACECLONE | PTRACE_O_TRACEEXEC | PTRACE_O_EXITKILL;
    assert(ptrace(PTRACE_SETOPTIONS, child, 0, (void*)options) == 0);
    assert(ptrace(PTRACE_CONT, child, 0, 0) == 0);
    for (;;) {
        pid_t thread = waitpid(-1, &status, __WALL);
        if (thread < 0 && errno == EINTR) {
            continue;
        }
        assert(thread > 0);
        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            if (thread == child) {
                assert(injected == 1);
                return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
            }
            continue;
        }
        assert(WIFSTOPPED(status));
        int event = status >> 16, deliver = WSTOPSIG(status);
        if (event == PTRACE_EVENT_SECCOMP) {
            struct user_regs_struct registers;
            assert(ptrace(PTRACE_GETREGS, thread, 0, &registers) == 0);
            assert(registers.orig_rax == (unsigned)number);
            // Select the guest's inherited input even after duplication;
            // startup reads and the eventfd used for signals must succeed.
            int selected = number != 0;
            if (number == 0) {
                char path[96];
                snprintf(path, sizeof(path), "/proc/%ld/fd/%llu", (long)thread, (unsigned long long)registers.rdi);
                struct stat target;
                selected = stat(path, &target) == 0 && target.st_dev == input.st_dev && target.st_ino == input.st_ino;
            }
            if (!injected && selected) {
                registers.orig_rax = (unsigned long long)-1;
                registers.rax = (unsigned long long)-EINTR;
                assert(ptrace(PTRACE_SETREGS, thread, 0, &registers) == 0);
                injected = 1;
            }
        }
        if (event || deliver == SIGSTOP) {
            deliver = 0;
        }
        assert(ptrace(PTRACE_CONT, thread, 0, (void*)(long)deliver) == 0);
    }
}
