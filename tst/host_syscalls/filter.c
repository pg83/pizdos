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
    while (*next) {
        char* end;
        long number = strtol(next, &end, 10);
        if (end == next || number < 0 || number > 1024 || count + 3 > sizeof(filters) / sizeof(filters[0]) || (*end && *end != ',')) {
            return 2;
        }
        filters[count++] = (struct Filter){0x15, 0, 1, (uint32_t)number};
        filters[count++] = (struct Filter){0x06, 0, 0, 0x00050000 | EIO};
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
