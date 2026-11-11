#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

static volatile sig_atomic_t interrupted;

static void alarmHandler(int signal) {
    interrupted = signal;
}

int main(int argc, char** argv) {
    assert(argc == 2);
    if (!strcmp(argv[1], "missing")) {
        assert(getpass("unreachable: ") == NULL);
        assert(errno == ENXIO || errno == ENODEV);
        puts("missing terminal: ok");
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
