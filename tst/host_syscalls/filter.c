#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

// The host has no kernel headers: what the filter takes of linux/filter.h,
// linux/seccomp.h and linux/audit.h, by their names.
struct sock_filter {
    uint16_t code;
    uint8_t jt, jf;
    uint32_t k;
};

struct sock_fprog {
    unsigned short len;
    struct sock_filter* filter;
};

struct seccomp_data {
    int nr;
    uint32_t arch;
    uint64_t instruction_pointer;
    uint64_t args[6];
};

#define BPF_LD 0x00
#define BPF_W 0x00
#define BPF_ABS 0x20
#define BPF_JMP 0x05
#define BPF_JEQ 0x10
#define BPF_JSET 0x40
#define BPF_K 0x00
#define BPF_RET 0x06
#define BPF_STMT(code, k) {(unsigned short)(code), 0, 0, k}
#define BPF_JUMP(code, k, jt, jf) {(unsigned short)(code), jt, jf, k}
#define SECCOMP_MODE_FILTER 2
#define SECCOMP_RET_KILL_PROCESS 0x80000000U
#define SECCOMP_RET_ERRNO 0x00050000U
#define SECCOMP_RET_ALLOW 0x7fff0000U
#define AUDIT_ARCH_X86_64 0xc000003eU

// The filter's statements: a word of seccomp_data into the accumulator, a
// jump over jt or jf statements as it is k or not, as it has a bit of k
// or not, and the end with a verdict.
#define LOAD(field) BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, field))
#define IS(k, jt, jf) BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, k, jt, jf)
#define HAS(k, jt, jf) BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, k, jt, jf)
#define RETURN(k) BPF_STMT(BPF_RET | BPF_K, k)

int main(int argc, char** argv) {
    if (argc < 4 || strcmp(argv[2], "--") != 0) {
        return 2;
    }
    // Linux amd64 only, then by the call's number.
    struct sock_filter filters[128] = {
        LOAD(arch), IS(AUDIT_ARCH_X86_64, 1, 0),
        RETURN(SECCOMP_RET_KILL_PROCESS), LOAD(nr),
    };
    unsigned count = 4;
    char* next = argv[1];
    unsigned error = EIO;
    char* slash = strchr(next, '/');
    if (slash != NULL) {
        char* end;
        long value = strtol(next, &end, 10);
        if (end != slash || value < 0 || value > 4095) {
            return 2;
        }
        error = (unsigned)value;
        next = slash + 1;
    }
    if (strcmp(next, "fcntl") == 0) {
        // Reject OFD lock queries, and F_SETFL when it requests O_APPEND.
        const struct sock_filter selected[] = {
            IS(SYS_fcntl, 0, 6), LOAD(args[1]), IS(F_OFD_GETLK, 3, 0),
            IS(F_SETFL, 0, 3), LOAD(args[2]), HAS(O_APPEND, 0, 1),
            RETURN(SECCOMP_RET_ERRNO | error),
        };
        for (unsigned i = 0; i < sizeof(selected) / sizeof(selected[0]); ++i) {
            filters[count++] = selected[i];
        }
        next += strlen(next);
    }
    if (strcmp(next, "stdin-stat") == 0) {
        // Limit the failure to stdin; loading the executable uses other fds.
        const struct sock_filter selected[] = {
            IS(SYS_fstat, 0, 3),
            LOAD(args[0]), IS(STDIN_FILENO, 0, 1),
            RETURN(SECCOMP_RET_ERRNO | error),
        };
        for (unsigned i = 0; i < sizeof(selected) / sizeof(selected[0]); ++i) {
            filters[count++] = selected[i];
        }
        next += strlen(next);
    }
    if (strcmp(next, "stdin-flags") == 0) {
        // Go checks F_GETFD at startup; reject only F_GETFL on stdin.
        const struct sock_filter selected[] = {
            IS(SYS_fcntl, 0, 5), LOAD(args[0]), IS(STDIN_FILENO, 0, 3),
            LOAD(args[1]), IS(F_GETFL, 0, 1),
            RETURN(SECCOMP_RET_ERRNO | error),
        };
        for (unsigned i = 0; i < sizeof(selected) / sizeof(selected[0]); ++i) {
            filters[count++] = selected[i];
        }
        next += strlen(next);
    }
    if (strcmp(next, "getfl") == 0) {
        // Guest descriptors may be duplicated onto high host descriptors.
        // Leave Go's F_GETFD startup checks available on every descriptor.
        const struct sock_filter selected[] = {
            IS(SYS_fcntl, 0, 3), LOAD(args[1]), IS(F_GETFL, 0, 1),
            RETURN(SECCOMP_RET_ERRNO | error),
        };
        for (unsigned i = 0; i < sizeof(selected) / sizeof(selected[0]); ++i) {
            filters[count++] = selected[i];
        }
        next += strlen(next);
    }
    if (strcmp(next, "pty-attributes") == 0) {
        // Let the PTY master and slave open, then fail only TCSETS.
        const struct sock_filter selected[] = {
            IS(SYS_ioctl, 0, 3), LOAD(args[1]), IS(TCSETS, 0, 1),
            RETURN(SECCOMP_RET_ERRNO | error),
        };
        for (unsigned i = 0; i < sizeof(selected) / sizeof(selected[0]); ++i) {
            filters[count++] = selected[i];
        }
        next += strlen(next);
    }
    if (strcmp(next, "stop") == 0) {
        // Deny only tgkill(..., SIGSTOP); Go's SIGURG preemption stays available.
        const struct sock_filter selected[] = {
            IS(SYS_tgkill, 0, 3), LOAD(args[2]), IS(SIGSTOP, 0, 1),
            RETURN(SECCOMP_RET_ERRNO | error),
        };
        for (unsigned i = 0; i < sizeof(selected) / sizeof(selected[0]); ++i) {
            filters[count++] = selected[i];
        }
        next += strlen(next);
    }
    while (*next) {
        char* end;
        long number = strtol(next, &end, 10);
        if (end == next || number < 0 || number > 1024 || count + 3 > sizeof(filters) / sizeof(filters[0]) || (*end && *end != ',')) {
            return 2;
        }
        filters[count++] = (struct sock_filter)IS((uint32_t)number, 0, 1);
        filters[count++] = (struct sock_filter)RETURN(SECCOMP_RET_ERRNO | error);
        next = *end ? end + 1 : end;
    }
    filters[count++] = (struct sock_filter)RETURN(SECCOMP_RET_ALLOW);
    struct sock_fprog program = {(unsigned short)count, filters};
    if (prctl(PR_SET_NO_NEW_PRIVS, 1UL, 0UL, 0UL, 0UL) != 0 || prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program, 0UL, 0UL) != 0) {
        perror("seccomp");
        return 1;
    }
    execvp(argv[3], argv + 3);
    perror("exec");
    return 1;
}
