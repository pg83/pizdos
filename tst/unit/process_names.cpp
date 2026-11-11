//@ compile-flags: -g -D_GNU_SOURCE
//@ stdout: "process names: ok\n"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

static void checkName(const char* expected) {
    char name[16];
    memset(name, '?', sizeof(name));
    assert(prctl(PR_GET_NAME, name) == 0 && name[15] == 0);
    assert(strcmp(name, expected) == 0);
}

int main() {
    char initial[16]{};
    assert(prctl(PR_GET_NAME, initial) == 0 && initial[0] && strlen(initial) <= 15);
    const char* names[] = {"parent-name", "", "0123456789abcdefghijkl"};
    const char* expected[] = {"parent-name", "", "0123456789abcde"};
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        assert(prctl(PR_SET_NAME, names[i]) == 0);
        checkName(expected[i]);
        pid_t child = fork();
        assert(child >= 0);
        if (child == 0) {
            checkName(expected[i]);
            assert(prctl(PR_SET_NAME, "child-only") == 0);
            checkName("child-only");
            _exit(17);
        }
        int status;
        assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 17);
        checkName(expected[i]);
    }
    puts("process names: ok");
}
