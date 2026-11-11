//@ host: false
//@ compile-flags: -g
//@ stdout: "pseudoterminals: ok\n"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

int main() {
    char name[128], actual[128];
    assert(grantpt(-1) == -1 && errno == EBADF);
    assert(unlockpt(-1) == -1 && errno == EBADF);
    assert(ptsname(-1) == nullptr && errno == EBADF);
    assert(ptsname_r(-1, name, sizeof(name)) == EBADF);
    int pipes[2];
    assert(pipe(pipes) == 0);
    assert(grantpt(pipes[0]) == -1 && errno == EINVAL);
    assert(unlockpt(pipes[0]) == -1 && errno == ENOTTY);
    assert(ptsname(pipes[0]) == nullptr && errno == ENOTTY);
    assert(ptsname_r(pipes[0], name, sizeof(name)) == ENOTTY);
    termios attributes{};
    assert(tcgetattr(pipes[0], &attributes) == -1 && errno == ENOTTY);
    assert(tcgetsid(pipes[0]) == -1 && errno == ENOTTY);
    assert(tcgetattr(-1, &attributes) == -1 && errno == EBADF);
    assert(tcsetattr(-1, TCSANOW, &attributes) == -1 && errno == EBADF);
    assert(tcdrain(-1) == -1 && errno == EBADF);
    assert(tcflush(pipes[0], TCIOFLUSH) == -1 && errno == ENOTTY);
    assert(close(pipes[0]) == 0 && close(pipes[1]) == 0);

    int master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    assert(master >= 0 && fcntl(master, F_GETFD) & FD_CLOEXEC);
    assert(grantpt(master) == 0 && unlockpt(master) == 0);
    assert(ptsname_r(master, name, 1) == ERANGE);
    assert(ptsname_r(master, name, sizeof(name)) == 0);
    assert(strcmp(ptsname(master), name) == 0 && strncmp(name, "/dev/pts/", 9) == 0);
    int slave = open(name, O_RDWR | O_NOCTTY);
    assert(slave >= 0 && isatty(slave) == 1);
    assert(tcsetpgrp(slave, 0x7fffffff) == -1 && errno == EPERM);
    assert(strcmp(ttyname(slave), name) == 0);
    assert(ttyname_r(slave, actual, 1) == ERANGE);
    assert(ttyname_r(slave, actual, sizeof(actual)) == 0 && strcmp(actual, name) == 0);
    assert(tcgetattr(slave, &attributes) == 0);
    assert(cfsetospeed(&attributes, B9600) == 0 && cfgetospeed(&attributes) == B9600);
    assert(cfsetispeed(&attributes, 0) == 0 && cfgetispeed(&attributes) == B9600);
    assert(cfsetispeed(&attributes, B115200) == 0 && cfgetispeed(&attributes) == B115200);
    assert(cfsetspeed(&attributes, B38400) == 0);
    assert(cfgetispeed(&attributes) == B38400 && cfgetospeed(&attributes) == B38400);
    assert(cfsetspeed(&attributes, 0xdeadbeef) == -1 && errno == EINVAL);
    cfmakeraw(&attributes);
    assert((attributes.c_lflag & (ECHO | ICANON | ISIG | IEXTEN)) == 0);
    assert((attributes.c_oflag & OPOST) == 0 && (attributes.c_cflag & CSIZE) == CS8);
    assert(attributes.c_cc[VMIN] == 1 && attributes.c_cc[VTIME] == 0);
    assert(tcsetattr(slave, 99, &attributes) == -1 && errno == EINVAL);
    assert(tcsetattr(slave, TCSANOW, &attributes) == 0);
    assert(tcsetattr(slave, TCSADRAIN, &attributes) == 0);
    assert(tcsetattr(slave, TCSAFLUSH, &attributes) == 0);
    assert(tcflow(slave, TCOOFF) == 0 && tcflow(slave, TCOON) == 0);
    assert(tcflow(slave, 99) == -1 && errno == EINVAL);
    assert(tcdrain(slave) == 0 && tcsendbreak(slave, 0) == 0);
    assert(tcflush(slave, TCIOFLUSH) == 0);
    assert(tcflush(slave, 99) == -1 && errno == EINVAL);
    assert(tcgetsid(slave) == getsid(0));
    assert(write(master, "abc", 3) == 3);
    assert(read(slave, actual, 3) == 3 && memcmp(actual, "abc", 3) == 0);
    assert(write(slave, "def", 3) == 3);
    assert(read(master, actual, 3) == 3 && memcmp(actual, "def", 3) == 0);
    assert(close(master) == 0 && close(slave) == 0);

    winsize dimensions{37, 89, 0, 0}, observed{};
    assert(openpty(&master, &slave, name, &attributes, &dimensions) == 0);
    assert(ioctl(slave, TIOCGWINSZ, &observed) == 0);
    assert(observed.ws_row == 37 && observed.ws_col == 89);
    assert(tcgetattr(slave, &attributes) == 0 && (attributes.c_lflag & ICANON) == 0);
    assert(strcmp(ttyname(slave), name) == 0);
    assert(close(master) == 0 && close(slave) == 0);
    assert(openpty(&master, &slave, nullptr, nullptr, nullptr) == 0);
    assert(close(master) == 0 && close(slave) == 0);

    rlimit limit{}, saved{};
    assert(getrlimit(RLIMIT_NOFILE, &saved) == 0);
    limit = saved;
    limit.rlim_cur = 3;
    assert(setrlimit(RLIMIT_NOFILE, &limit) == 0);
    assert(openpty(&master, &slave, nullptr, nullptr, nullptr) == -1 && errno == EMFILE);
    limit.rlim_cur = 4;
    assert(setrlimit(RLIMIT_NOFILE, &limit) == 0);
    assert(openpty(&master, &slave, nullptr, nullptr, nullptr) == -1 && errno == EMFILE);
    assert(setrlimit(RLIMIT_NOFILE, &saved) == 0);

    pid_t child = forkpty(&master, name, &attributes, &dimensions);
    assert(child >= 0);
    if (child == 0) {
        assert(isatty(0) && isatty(1) && isatty(2));
        assert(getsid(0) == getpid() && getpgrp() == getpid());
        char byte;
        assert(read(0, &byte, 1) == 1 && byte == 'a');
        assert(write(1, "ack", 3) == 3);
        _exit(17);
    }
    assert(write(master, "a", 1) == 1);
    size_t received = 0;
    while (received < 3) {
        ssize_t count = read(master, actual + received, 3 - received);
        assert(count > 0);
        received += size_t(count);
    }
    assert(memcmp(actual, "ack", 3) == 0);
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 17);
    assert(close(master) == 0);
    puts("pseudoterminals: ok");
}
