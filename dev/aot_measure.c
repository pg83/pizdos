#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Fork from a small native process: a Python parent's resident memory can
   otherwise become the child's inherited ru_maxrss before exec. */
int main(int argc, char** argv) {
    if (argc < 2) {
        return 2;
    }
    struct timespec begin, end;
    clock_gettime(CLOCK_MONOTONIC, &begin);
    pid_t child = fork();
    if (child < 0) {
        return 2;
    }
    if (child == 0) {
        execv(argv[1], argv + 1);
        _exit(127);
    }
    int status;
    struct rusage usage;
    pid_t result;
    do {
        result = wait4(child, &status, 0, &usage);
    } while (result < 0 && errno == EINTR);
    if (result < 0) {
        return 2;
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    double wall = end.tv_sec - begin.tv_sec + (end.tv_nsec - begin.tv_nsec) * 1e-9;
    double user = usage.ru_utime.tv_sec + usage.ru_utime.tv_usec * 1e-6;
    double system = usage.ru_stime.tv_sec + usage.ru_stime.tv_usec * 1e-6;
    fprintf(stderr, "AOT_MEASURE {\"wall\":%.9f,\"user\":%.9f,\"system\":%.9f,\"rss_kib\":%ld}\n", wall, user, system, usage.ru_maxrss);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}
