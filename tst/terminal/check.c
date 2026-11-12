#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

static volatile sig_atomic_t interrupted;

static void alarmHandler(int signal) {
    interrupted = signal;
}

int main(int argc, char** argv) {
    assert(argc == 2);
    if (!strcmp(argv[1], "private-name")) {
        char* name = ttyname(0);
        assert(name && !strncmp(name, "/ttys/", 6) && name[6] >= '0' && name[6] <= '9');
        char copied[128];
        assert(ttyname_r(0, copied, sizeof(copied)) == 0 && !strcmp(name, copied));
        int reopened = open(copied, O_RDWR | O_NOCTTY);
        struct termios inherited, actual;
        assert(reopened >= 0 && tcgetattr(0, &inherited) == 0 && tcgetattr(reopened, &actual) == 0);
        assert(inherited.c_lflag == actual.c_lflag && close(reopened) == 0);
        puts("private terminal name: ok");
        return 0;
    }
    if (!strcmp(argv[1], "missing")) {
        assert(getpass("unreachable: ") == NULL);
        assert(errno == ENXIO || errno == ENODEV);
        puts("missing terminal: ok");
        return 0;
    }
    if (!strcmp(argv[1], "attributes")) {
        assert(getpass("unreachable: ") == NULL && errno == ENOTTY);
        puts("terminal attributes: failed as expected");
        return 0;
    }
    if (!strcmp(argv[1], "interrupt")) {
        struct sigaction action = {0};
        action.sa_handler = alarmHandler;
        assert(sigemptyset(&action.sa_mask) == 0 && sigaction(SIGALRM, &action, NULL) == 0);
        ualarm(10000, 0);
        assert(getpass("interrupt: ") == NULL && errno == EINTR);
        assert(interrupted == SIGALRM);
        puts("interrupted terminal: ok");
        return 0;
    }
    struct termios before, after;
    assert(tcgetattr(0, &before) == 0);
    pid_t foreground = -1;
    assert(ioctl(0, TIOCGPGRP, &foreground) == 0);
    assert(foreground == getpgrp() && foreground == tcgetpgrp(0));
    char* password = getpass("first: ");
    assert(password && !strcmp(password, "s3cr3t"));
    assert(tcgetattr(0, &after) == 0 && before.c_lflag == after.c_lflag);
    password = getpass("empty: ");
    assert(password && password[0] == 0);
    password = getpass("long: ");
    assert(password && strlen(password) == 127);
    for (unsigned i = 0; i < 127; ++i) {
        assert(password[i] == 'x');
    }
    password = getpass("eof: ");
    assert(password && password[0] == 0);
    assert(tcgetattr(0, &after) == 0 && before.c_lflag == after.c_lflag);
    puts("terminal passwords: ok");
    return 0;
}
