//@ compile-flags: -g -D_GNU_SOURCE
//@ stdout: "process groups: ok\n"

#include <assert.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

int main() {
    int release[2];
    assert(pipe(release) == 0);
    const pid_t group = getpgrp();
    const pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        char byte;
        assert(close(release[1]) == 0 && read(release[0], &byte, 1) == 1 && byte == 'r');
        assert(getpgrp() == group);
        _exit(17);
    }
    assert(close(release[0]) == 0);
    assert(setpgid(child, 0) == 0 && getpgid(child) == child);
    assert(setpgid(child, group) == 0 && getpgid(child) == group);
    assert(write(release[1], "r", 1) == 1 && close(release[1]) == 0);
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 17);
    puts("process groups: ok");
}
