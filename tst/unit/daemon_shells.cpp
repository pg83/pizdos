//@ host: false
//@ compile-flags: -g
//@ stdout: "daemon and shells: ok\n"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static void waitFor(pid_t pid, int code) {
    int status;
    assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == code);
}

static void transfer(int fd, void* data, size_t length, bool writing) {
    auto* bytes = static_cast<char*>(data);
    while (length) {
        ssize_t n = writing ? write(fd, bytes, length) : read(fd, bytes, length);
        assert(n > 0);
        bytes += n;
        length -= size_t(n);
    }
}

int main() {
    setusershell();
    assert(strcmp(getusershell(), "/bin/sh") == 0);
    assert(strcmp(getusershell(), "/bin/csh") == 0);
    assert(getusershell() == nullptr);
    endusershell();
    assert(strcmp(getusershell(), "/bin/sh") == 0);
    assert(mkdir("/etc", 0755) == 0);
    FILE* file = fopen("/etc/shells", "w");
    assert(file);
    fputc('#', file);
    for (unsigned i = 0; i < 5000; ++i) {
        fputc('x', file);
    }
    fputs("\n\n   # ignored\n  /bin/first  \n\t/bin/second\n", file);
    assert(fclose(file) == 0);
    setusershell();
    assert(strcmp(getusershell(), "/bin/first") == 0);
    assert(strcmp(getusershell(), "/bin/second") == 0);
    assert(getusershell() == nullptr && getusershell() == nullptr);
    setusershell();
    assert(strcmp(getusershell(), "/bin/first") == 0);
    endusershell();
    assert(strcmp(getusershell(), "/bin/first") == 0);
    assert(unlink("/etc/shells") == 0 && mkdir("/etc/shells", 0755) == 0);
    setusershell();
    assert(getusershell() == nullptr);
    assert(rmdir("/etc/shells") == 0);

    int reports[2], release[2];
    assert(pipe(reports) == 0 && pipe(release) == 0);
    pid_t original = getpid();
    assert(chdir("/tmp") == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        pid_t old = getpid();
        pid_t grandchild = fork();
        assert(grandchild >= 0);
        if (grandchild == 0) {
            char byte;
            transfer(release[0], &byte, 1, false);
            assert(byte == 'x' && getppid() == original);
            _exit(19);
        }
        assert(daemon(0, 0) == 0);
        assert(getpid() != old && getsid(0) == getpid() && getpgrp() == getpid());
        assert(getppid() == original);
        char cwd[16], byte;
        assert(getcwd(cwd, sizeof(cwd)) && strcmp(cwd, "/") == 0);
        assert(read(0, &byte, 1) == 0);
        assert(write(1, "hidden", 6) == 6 && write(2, "hidden", 6) == 6);
        pid_t report[] = {getpid(), grandchild};
        transfer(reports[1], report, sizeof(report), true);
        _exit(17);
    }
    pid_t report[2];
    transfer(reports[0], report, sizeof(report), false);
    assert(report[0] != child && report[1] != child && report[0] != report[1]);
    waitFor(child, 0);
    waitFor(report[0], 17);
    char byte = 'x';
    transfer(release[1], &byte, 1, true);
    waitFor(report[1], 19);

    assert(signal(SIGCHLD, SIG_IGN) != SIG_ERR);
    child = fork();
    assert(child >= 0);
    if (child == 0) {
        assert(daemon(1, 1) == 0);
        char cwd[16];
        assert(getcwd(cwd, sizeof(cwd)) && strcmp(cwd, "/tmp") == 0);
        transfer(reports[1], &byte, 1, true);
        _exit(0);
    }
    transfer(reports[0], &byte, 1, false);
    int status;
    assert(waitpid(child, &status, WNOHANG) == -1 && errno == ECHILD);
    assert(signal(SIGCHLD, SIG_DFL) == SIG_IGN);
    for (int fd : reports) {
        assert(close(fd) == 0);
    }
    for (int fd : release) {
        assert(close(fd) == 0);
    }
    // The initial process remains the VM's lifetime owner after daemon().
    assert(daemon(0, 1) == 0);
    char cwd[16];
    assert(getcwd(cwd, sizeof(cwd)) && strcmp(cwd, "/") == 0);
    puts("daemon and shells: ok");
}
