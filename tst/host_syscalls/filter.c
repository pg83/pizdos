#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
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

int main(int argc, char** argv) {
    if (argc < 4 || strcmp(argv[2], "--") != 0) {
        return 2;
    }
    // seccomp_data: architecture at 4, syscall number at 0, Linux amd64.
    struct Filter filters[128] = {
        {0x20, 0, 0, 4}, {0x15, 1, 0, 0xc000003e},
        {0x06, 0, 0, 0x80000000}, {0x20, 0, 0, 0},
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
        const struct Filter selected[] = {
            {0x15, 0, 6, 72}, {0x20, 0, 0, 24}, {0x15, 3, 0, 36},
            {0x15, 0, 3, 4}, {0x20, 0, 0, 32}, {0x45, 0, 1, 1024},
            {0x06, 0, 0, 0x00050000 | error},
        };
        for (unsigned i = 0; i < sizeof(selected) / sizeof(selected[0]); ++i) {
            filters[count++] = selected[i];
        }
        next += strlen(next);
    }
    if (strcmp(next, "stdin-stat") == 0) {
        // Limit the failure to stdin; loading the executable uses other fds.
        const struct Filter selected[] = {
            {0x15, 0, 3, 5},
            {0x20, 0, 0, 16}, {0x15, 0, 1, 0},
            {0x06, 0, 0, 0x00050000 | error},
        };
        for (unsigned i = 0; i < sizeof(selected) / sizeof(selected[0]); ++i) {
            filters[count++] = selected[i];
        }
        next += strlen(next);
    }
    if (strcmp(next, "stdin-flags") == 0) {
        // Go checks F_GETFD at startup; reject only F_GETFL on stdin.
        const struct Filter selected[] = {
            {0x15, 0, 5, 72}, {0x20, 0, 0, 16}, {0x15, 0, 3, 0},
            {0x20, 0, 0, 24}, {0x15, 0, 1, 3},
            {0x06, 0, 0, 0x00050000 | error},
        };
        for (unsigned i = 0; i < sizeof(selected) / sizeof(selected[0]); ++i) {
            filters[count++] = selected[i];
        }
        next += strlen(next);
    }
    if (strcmp(next, "getfl") == 0) {
        // Guest descriptors may be duplicated onto high host descriptors.
        // Leave Go's F_GETFD startup checks available on every descriptor.
        const struct Filter selected[] = {
            {0x15, 0, 3, 72}, {0x20, 0, 0, 24}, {0x15, 0, 1, 3},
            {0x06, 0, 0, 0x00050000 | error},
        };
        for (unsigned i = 0; i < sizeof(selected) / sizeof(selected[0]); ++i) {
            filters[count++] = selected[i];
        }
        next += strlen(next);
    }
    if (strcmp(next, "pty-attributes") == 0) {
        // Let the PTY master and slave open, then fail only TCSETS.
        const struct Filter selected[] = {
            {0x15, 0, 3, 16}, {0x20, 0, 0, 24}, {0x15, 0, 1, 0x5402},
            {0x06, 0, 0, 0x00050000 | error},
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
        filters[count++] = (struct Filter){0x15, 0, 1, (uint32_t)number};
        filters[count++] = (struct Filter){0x06, 0, 0, 0x00050000 | error};
        next = *end ? end + 1 : end;
    }
    filters[count++] = (struct Filter){0x06, 0, 0, 0x7fff0000};
    struct Program program = {(unsigned short)count, filters};
    if (prctl(PR_SET_NO_NEW_PRIVS, 1UL, 0UL, 0UL, 0UL) != 0 || prctl(PR_SET_SECCOMP, 2UL, &program, 0UL, 0UL) != 0) {
        perror("seccomp");
        return 1;
    }
    execvp(argv[3], argv + 3);
    perror("exec");
    return 1;
}
