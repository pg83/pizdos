//@ host: false
//@ compile-flags: -O2 -D_GNU_SOURCE
//@ stdout: "structure sizes: ok\n"
//@ stderr-pattern: "(?s)(?:.*?a structure of \\d+ bytes, which this kernel knows of \\d+){8}.*"

// Every structure lib/c hands the kernel comes with its size, its version,
// which the kernel checks against the layouts it knows. A call with
// another size, as a program built for a layout this kernel never knew
// would make, ends the process by SIGSYS before the kernel touches the
// structure. The imports are called here as lib/c does, with sizes of
// our own.

#include <assert.h>
#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <regex.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern "C" {
int __kernel_stat(const char*, struct stat*, size_t) __KERNEL(stat);
struct passwd* __kernel_getpwnam(const char*, size_t) __KERNEL(getpwnam);
void __kernel_regfree(regex_t*, size_t) __KERNEL(regfree);
int __kernel_sigaction(int, const struct sigaction*, size_t, struct sigaction*, size_t, size_t) __KERNEL(sigaction);
int __kernel_fcntl(int, int, va_list, size_t) __KERNEL(fcntl);
int __kernel_ioctl(int, unsigned long, va_list, size_t) __KERNEL(ioctl);
int __kernel_poll(struct pollfd*, size_t, nfds_t, int) __KERNEL(poll);
}

static int fcntlSized(int fd, int cmd, size_t size, ...) {
    va_list ap;
    va_start(ap, size);
    int r = __kernel_fcntl(fd, cmd, ap, size);
    va_end(ap);
    return r;
}

static int ioctlSized(int fd, unsigned long request, size_t size, ...) {
    va_list ap;
    va_start(ap, size);
    int r = __kernel_ioctl(fd, request, ap, size);
    va_end(ap);
    return r;
}

// refused runs call in a child, which must end by SIGSYS.
static void refused(void (*call)()) {
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        call();
        _exit(0);
    }
    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGSYS);
}

int main() {
    // The size lib/c passes works, through the import as through lib/c.
    struct stat st = {};
    assert(__kernel_stat("/", &st, sizeof st) == 0 && S_ISDIR(st.st_mode));

    refused([] {
        struct stat st[2];
        __kernel_stat("/", st, sizeof st[0] - 8);
    });
    refused([] {
        struct stat st[2];
        __kernel_stat("/", st, sizeof st[0] + 8);
    });
    refused([] {
        __kernel_getpwnam("root", sizeof(struct passwd) + 4);
    });
    refused([] {
        regex_t re;
        assert(regcomp(&re, "a", 0) == 0);
        __kernel_regfree(&re, sizeof re - 4);
    });
    refused([] {
        struct sigaction action = {};
        action.sa_handler = SIG_IGN;
        __kernel_sigaction(SIGUSR1, &action, sizeof action, nullptr, sizeof action, sizeof(siginfo_t) + 4);
    });
    refused([] {
        struct flock lock = {};
        lock.l_type = F_RDLCK;
        fcntlSized(0, F_GETLK, sizeof lock - 4, &lock);
    });
    refused([] {
        struct winsize size;
        ioctlSized(0, TIOCGWINSZ, 0, &size);
    });
    refused([] {
        struct pollfd fds[2] = {};
        fds[0].fd = 0;
        __kernel_poll(fds, 2 * sizeof fds[0], 1, 0);
    });
    printf("structure sizes: ok\n");
    return 0;
}
