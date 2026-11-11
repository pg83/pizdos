//@ compile-flags: -g -D_GNU_SOURCE -fno-builtin
//@ stdout: "diagnostic output: ok\n"

#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

static void returningHandler(int sig) {
    const char text[] = "abort handler\n";
    if (sig != SIGABRT || write(2, text, sizeof(text) - 1) != sizeof(text) - 1) {
        _exit(99);
    }
}

int main() {
    rlimit core{};
    assert(setrlimit(RLIMIT_CORE, &core) == 0);
    for (int mode = 0; mode != 5; ++mode) {
        int channel[2];
        assert(pipe(channel) == 0);
        pid_t child = fork();
        assert(child >= 0);
        if (child == 0) {
            assert(close(channel[0]) == 0);
            assert(dup2(channel[1], 2) == 2 && close(channel[1]) == 0);
            if (mode == 0 || mode == 1) {
                if (mode == 1) {
                    assert(close(2) == 0);
                }
                __assert_fail("requested failure", "diagnostic.cpp", 37, "probe");
            }
            if (mode == 2) {
                assert(signal(SIGABRT, returningHandler) != SIG_ERR);
                abort();
            }
            if (mode == 3) {
                assert(signal(SIGABRT, SIG_IGN) != SIG_ERR);
                sigset_t blocked;
                assert(sigemptyset(&blocked) == 0 && sigaddset(&blocked, SIGABRT) == 0);
                assert(sigprocmask(SIG_BLOCK, &blocked, nullptr) == 0);
                abort();
            }
            psignal(SIGUSR1, "prefix");
            psignal(SIGTERM, nullptr);
            siginfo_t info{};
            info.si_signo = SIGUSR2;
            psiginfo(&info, "siginfo");
            assert(close(2) == 0);
            psignal(SIGUSR1, "closed");
            psiginfo(&info, "closed");
            _exit(0);
        }
        assert(close(channel[1]) == 0);
        char text[1024];
        size_t used = 0;
        for (;;) {
            ssize_t n = read(channel[0], text + used, sizeof(text) - 1 - used);
            assert(n >= 0);
            if (!n) {
                break;
            }
            used += size_t(n);
            assert(used < sizeof(text) - 1);
        }
        text[used] = 0;
        assert(close(channel[0]) == 0);
        int status;
        assert(waitpid(child, &status, 0) == child);
        if (mode == 4) {
            assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
            assert(strncmp(text, "prefix: ", 8) == 0);
            assert(strstr(text, strsignal(SIGUSR1)) && strstr(text, strsignal(SIGTERM)));
            const char* info = strstr(text, "siginfo: ");
            assert(info && strstr(info, strsignal(SIGUSR2)));
        } else {
            assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
            if (mode == 0) {
                assert(strcmp(text, "Assertion failed: requested failure (diagnostic.cpp: probe: 37)\n") == 0);
            } else if (mode == 2) {
                assert(strcmp(text, "abort handler\n") == 0);
            } else {
                assert(used == 0);
            }
        }
    }
    puts("diagnostic output: ok");
}
