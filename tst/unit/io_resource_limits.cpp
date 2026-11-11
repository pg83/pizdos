//@ compile-flags: -g -D_GNU_SOURCE -fno-builtin
//@ stdout: "I/O resource limits: ok\n"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <termios.h>
#include <unistd.h>

int main() {
    rlimit saved, limit;
    assert(getrlimit(RLIMIT_NOFILE, &saved) == 0 && saved.rlim_cur >= 5);
    limit = saved;
    for (rlim_t count = 3; count != 5; ++count) {
        limit.rlim_cur = count;
        assert(setrlimit(RLIMIT_NOFILE, &limit) == 0);
        int ends[2] = {-1, -1};
        assert(pipe2(ends, O_CLOEXEC | O_NONBLOCK) == -1 && errno == EMFILE);
        assert(ends[0] == -1 && ends[1] == -1);
        assert(dup2(1, int(count)) == -1 && errno == EBADF);
        if (count == 3) {
            assert(dup(1) == -1 && errno == EMFILE);
            assert(open("/dev/null", O_RDONLY) == -1 && errno == EMFILE);
        } else {
            // A failed pipe must release the first end it installed.
            int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
            assert(fd == 3 && (fcntl(fd, F_GETFD) & FD_CLOEXEC));
            assert(close(fd) == 0);
        }
    }
    assert(setrlimit(RLIMIT_NOFILE, &saved) == 0);
    int ends[2] = {-1, -1};
    assert(pipe2(ends, O_APPEND) == -1 && errno == EINVAL);
    assert(pipe(ends) == 0 && ends[0] == 3 && ends[1] == 4);
    assert(close(ends[0]) == 0 && close(ends[1]) == 0);

    int flags = fcntl(1, F_GETFL);
    assert(flags >= 0);
    assert(fcntl(1, F_SETFL, flags | O_NONBLOCK) == 0);
    assert(fcntl(1, F_GETFL) & O_NONBLOCK);
    assert(fcntl(1, F_SETFL, flags) == 0);

    int fd = open("/dev/null", O_RDWR);
    assert(fd >= 0);
    termios settings{};
    assert(ioctl(fd, TCGETS, &settings) == -1 && errno == ENOTTY);
    assert(ioctl(fd, TCSETS, &settings) == -1 && errno == ENOTTY);
    assert(ioctl(fd, TCSETSW, &settings) == -1 && errno == ENOTTY);
    assert(ioctl(fd, TCSETSF, &settings) == -1 && errno == ENOTTY);
    int group = getpgrp();
    assert(ioctl(fd, TIOCGPGRP, &group) == -1 && errno == ENOTTY);
    assert(ioctl(fd, TIOCSPGRP, &group) == -1 && errno == ENOTTY);
    winsize size{};
    assert(ioctl(fd, TIOCGWINSZ, &size) == -1 && errno == ENOTTY);
    assert(ioctl(fd, TIOCSWINSZ, &size) == -1 && errno == ENOTTY);
    int available = 0;
    assert(ioctl(fd, FIONREAD, &available) == -1 && errno == ENOTTY);
    assert(ioctl(fd, 0xffffffffUL, &available) == -1 && errno == ENOTTY);
    assert(close(fd) == 0);
    puts("I/O resource limits: ok");
}
