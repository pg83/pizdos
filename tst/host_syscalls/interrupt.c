#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
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

// Occupy the first name selected by mktemp before its real existence check.
// The guest must leave this file intact and return a different unused name.
static int occupy_candidate(pid_t thread, const struct user_regs_struct* registers) {
    char path[32] = {0};
    for (unsigned offset = 0; offset < sizeof(path); offset += sizeof(long)) {
        errno = 0;
        long word = ptrace(PTRACE_PEEKDATA, thread, (void*)(registers->rsi + offset), 0);
        assert(word != -1 || errno == 0);
        memcpy(path + offset, &word, sizeof(word));
        if (memchr(&word, 0, sizeof(word))) {
            break;
        }
    }
    if (strncmp(path, "/tmp/collision-", 15) != 0 || strnlen(path, sizeof(path)) != 21) {
        return 0;
    }
    char descriptor[96];
    snprintf(descriptor, sizeof(descriptor), "/proc/%ld/fd/%llu", (long)thread, (unsigned long long)registers->rdi);
    int root = open(descriptor, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    assert(root >= 0);
    int file = openat(root, path + 1, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    assert(file >= 0 && write(file, "occupied", 8) == 8);
    assert(close(file) == 0 && close(root) == 0);
    return 1;
}

// Route DNS sockets to the test's loopback responder, preserving real UDP
// traffic. Other connections and all socket return values stay the host's.
static int redirect_dns(pid_t thread, uintptr_t at, unsigned port) {
    unsigned char bytes[32];
    errno = 0;
    long first = ptrace(PTRACE_PEEKDATA, thread, (void*)at, 0);
    assert(first != -1 || errno == 0);
    memcpy(bytes, &first, sizeof(first));
    uint16_t family, original_port;
    memcpy(&family, bytes, 2);
    memcpy(&original_port, bytes + 2, 2);
    if ((family != AF_INET && family != AF_INET6) || original_port != htons(53)) {
        return 0;
    }
    unsigned size = family == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);
    for (unsigned offset = sizeof(long); offset < size; offset += sizeof(long)) {
        errno = 0;
        long word = ptrace(PTRACE_PEEKDATA, thread, (void*)(at + offset), 0);
        assert(word != -1 || errno == 0);
        memcpy(bytes + offset, &word, sizeof(word));
    }
    if (family == AF_INET) {
        struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
        memcpy(bytes, &address, sizeof(address));
    } else {
        struct sockaddr_in6 address = {.sin6_family = AF_INET6, .sin6_port = htons(port), .sin6_addr = in6addr_loopback};
        memcpy(bytes, &address, sizeof(address));
    }
    for (unsigned offset = 0; offset < size; offset += sizeof(long)) {
        long word;
        memcpy(&word, bytes + offset, sizeof(word));
        assert(ptrace(PTRACE_POKEDATA, thread, (void*)(at + offset), (void*)word) == 0);
    }
    return 1;
}

// Return EINTR once at the real syscall boundary, then let the host handle
// every retry. Seccomp limits ptrace stops to the selected syscall; traced
// Go threads inherit the filter and tracing through PTRACE_O_TRACECLONE.
// dns/<port> routes DNS connections to a real local responder instead;
// collision creates a real file before mktemp checks its first candidate.
int main(int argc, char** argv) {
    assert(argc >= 5 && strcmp(argv[2], "--") == 0);
    unsigned dns_port = strncmp(argv[1], "dns/", 4) == 0 ? atoi(argv[1] + 4) : 0;
    assert(dns_port <= 65535);
    int collision = strcmp(argv[1], "collision") == 0;
    int number = dns_port ? 42 : collision ? 437 : atoi(argv[1]);
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
                assert(dns_port ? injected > 0 : injected == 1);
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
            // startup reads must succeed.
            int selected = number != 0;
            if (number == 0) {
                char path[96];
                snprintf(path, sizeof(path), "/proc/%ld/fd/%llu", (long)thread, (unsigned long long)registers.rdi);
                struct stat target;
                selected = stat(path, &target) == 0 && target.st_dev == input.st_dev && target.st_ino == input.st_ino;
            }
            if (dns_port) {
                injected += redirect_dns(thread, registers.rsi, dns_port);
            } else if (collision) {
                if (!injected) {
                    injected = occupy_candidate(thread, &registers);
                }
            } else if (!injected && selected) {
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
