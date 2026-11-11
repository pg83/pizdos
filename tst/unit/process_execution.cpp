//@ host: false
//@ compile-flags: -g -D_GNU_SOURCE
//@ stdout: "process execution: ok\n"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static void unusedHandler(int) {
    abort();
}

static void copy(const char* from, const char* to, mode_t mode) {
    int input = open(from, O_RDONLY), output = open(to, O_WRONLY | O_CREAT | O_TRUNC, mode);
    assert(input >= 0 && output >= 0);
    char bytes[4096];
    ssize_t length;
    while ((length = read(input, bytes, sizeof(bytes))) > 0) {
        ssize_t written = 0;
        while (written < length) {
            ssize_t n = write(output, bytes + written, length - written);
            assert(n > 0);
            written += n;
        }
    }
    assert(length == 0 && close(input) == 0 && close(output) == 0);
}

static void script(const char* path, const char* text) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0700);
    assert(fd >= 0 && write(fd, text, strlen(text)) == ssize_t(strlen(text)) && close(fd) == 0);
}

static void waitFor(pid_t child, int expected) {
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == expected);
}

int main(int argc, char** argv) {
    if (argc == 3 && strcmp(argv[0], "sh") == 0 && strcmp(argv[1], "-c") == 0) {
        assert(strcmp(getenv("MARK"), "inherited") == 0);
        if (strcmp(argv[2], "signal") == 0) {
            raise(SIGTERM);
            return 1;
        }
        return strcmp(argv[2], "exit23") == 0 ? 23 : 0;
    }
    if (argc >= 2 && strcmp(argv[1], "interpreter-arg") == 0) {
        assert(argc == 4 && strcmp(argv[2], "/tmp/script") == 0 && strcmp(argv[3], "tail") == 0);
        return 23;
    }
    if (argc >= 2 && strcmp(argv[1], "child") == 0) {
        assert(argc == 3 && strcmp(argv[0], "renamed") == 0);
        const char* mark = getenv("MARK");
        assert(strcmp(argv[2], "empty") == 0 ? mark == nullptr : mark && strcmp(mark, argv[2]) == 0);
        struct sigaction action{};
        assert(sigaction(SIGUSR1, nullptr, &action) == 0 && action.sa_handler == SIG_DFL);
        sigset_t mask;
        assert(sigprocmask(SIG_SETMASK, nullptr, &mask) == 0 && sigismember(&mask, SIGUSR2));
        stack_t stack{};
        assert(sigaltstack(nullptr, &stack) == 0 && (stack.ss_flags & SS_DISABLE));
        return 17;
    }
    assert(setenv("MARK", "inherited", 1) == 0 && setenv("PATH", "/bin", 1) == 0);
    assert(signal(SIGUSR1, unusedHandler) == SIG_DFL);
    sigset_t blocked, saved;
    assert(sigemptyset(&blocked) == 0 && sigaddset(&blocked, SIGUSR2) == 0);
    assert(sigprocmask(SIG_BLOCK, &blocked, &saved) == 0);
    char alt[8192];
    stack_t stack{};
    stack.ss_sp = alt;
    stack.ss_size = sizeof(alt);
    assert(sigaltstack(&stack, nullptr) == 0);
    char* inherited[] = {const_cast<char*>("renamed"), const_cast<char*>("child"), const_cast<char*>("inherited"), nullptr};
    char* overridden[] = {inherited[0], inherited[1], const_cast<char*>("overridden"), nullptr};
    char* empty[] = {inherited[0], inherited[1], const_cast<char*>("empty"), nullptr};
    char* environment[] = {const_cast<char*>("MARK=overridden"), const_cast<char*>("PATH=/missing"), nullptr};
    for (unsigned variant = 0; variant < 8; ++variant) {
        pid_t child = fork();
        assert(child >= 0);
        if (child == 0) {
            switch (variant) {
                case 0: execv("/bin/check", inherited); break;
                case 1: execl("/bin/check", "renamed", "child", "inherited", nullptr); break;
                case 2: execve("/bin/check", overridden, environment); break;
                case 3: execle("/bin/check", "renamed", "child", "overridden", nullptr, environment); break;
                case 4: execlp("check", "renamed", "child", "inherited", nullptr); break;
                case 5: execvpe("check", overridden, environment); break;
                case 6: {
                    int fd = open("/bin/check", O_RDONLY | O_CLOEXEC);
                    assert(fd >= 0);
                    fexecve(fd, overridden, environment);
                    break;
                }
                case 7: execve("/bin/check", empty, nullptr); break;
            }
            abort();
        }
        waitFor(child, 17);
    }
    assert(execvp("", inherited) == -1 && errno == ENOENT);
    assert(execle("/missing", nullptr, environment) == -1 && errno == ENOENT);
    assert(execl("/missing", nullptr) == -1 && errno == ENOENT);
    assert(fexecve(-1, inherited, nullptr) == -1 && errno == EBADF);
    int dir = open("/", O_RDONLY | O_DIRECTORY);
    assert(dir >= 0 && fexecve(dir, inherited, nullptr) == -1 && errno == EACCES && close(dir) == 0);
    script("/tmp/empty", "");
    assert(execv("/tmp/empty", inherited) == -1 && errno == ENOEXEC);
    script("/tmp/bad", "#!   \n");
    assert(execv("/tmp/bad", inherited) == -1 && errno == ENOEXEC);
    script("/tmp/recursive", "#!/tmp/recursive\n");
    assert(execv("/tmp/recursive", inherited) == -1 && errno == ELOOP);
    script("/tmp/script", "#!/bin/check interpreter-arg\nignored\n");
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        execl("/tmp/script", "ignored", "tail", nullptr);
        abort();
    }
    waitFor(child, 23);

    assert(mkdir("/tmp/denied", 0700) == 0);
    copy("/bin/check", "/tmp/denied/check", 0600);
    assert(setenv("PATH", "/tmp/denied:/missing", 1) == 0);
    assert(execvp("check", inherited) == -1 && errno == EACCES);
    assert(setenv("PATH", "/tmp", 1) == 0);
    assert(execvp("recursive", inherited) == -1 && errno == ELOOP);
    child = fork();
    assert(child >= 0);
    if (child == 0) {
        assert(chdir("/bin") == 0 && setenv("PATH", ":/missing", 1) == 0);
        execvp("check", inherited);
        abort();
    }
    waitFor(child, 17);
    assert(system(nullptr) == 0);
    int status = system("exit23");
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 127);
    copy("/bin/check", "/bin/sh", 0700);
    assert(system(nullptr) != 0);
    status = system("exit23");
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 23);
    status = system("signal");
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);
    assert(sigprocmask(SIG_SETMASK, &saved, nullptr) == 0);
    stack.ss_flags = SS_DISABLE;
    assert(sigaltstack(&stack, nullptr) == 0);
    puts("process execution: ok");
}
