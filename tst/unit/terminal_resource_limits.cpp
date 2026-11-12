//@ compile-flags: -g -D_GNU_SOURCE
//@ stdout: "terminal resource limits: ok\n"

#include <assert.h>
#include <errno.h>
#include <pty.h>
#include <stdio.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

int main() {
    rlimit saved{}, limited{};
    assert(getrlimit(RLIMIT_NOFILE, &saved) == 0 && saved.rlim_cur >= 5);
    limited = saved;
    limited.rlim_cur = 3;
    assert(setrlimit(RLIMIT_NOFILE, &limited) == 0);
    int master = -1, slave = -1;
    assert(openpty(&master, &slave, nullptr, nullptr, nullptr) == -1 && errno == EMFILE);
    assert(master == -1 && slave == -1);
    errno = 0;
    pid_t child = forkpty(&master, nullptr, nullptr, nullptr);
    if (child == 0) {
        _exit(17);
    }
    assert(child == -1 && errno == EMFILE && master == -1);
    int status;
    assert(waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD);
    limited.rlim_cur = 4;
    assert(setrlimit(RLIMIT_NOFILE, &limited) == 0);
    assert(openpty(&master, &slave, nullptr, nullptr, nullptr) == -1 && errno == EMFILE);
    assert(master == -1 && slave == -1);
    int available = dup(1);
    assert(available == 3 && close(available) == 0);
    assert(setrlimit(RLIMIT_NOFILE, &saved) == 0);
    assert(openpty(&master, &slave, nullptr, nullptr, nullptr) == 0);
    assert(master >= 3 && slave >= 3 && close(master) == 0 && close(slave) == 0);
    puts("terminal resource limits: ok");
}
