//@ compile-flags: -g -D_GNU_SOURCE
//@ stdout: "vfork execution: ok\n"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char** argv) {
    if (argc == 2) {
        return 23;
    }
    char* arguments[] = {argv[0], const_cast<char*>("child"), nullptr};
#ifdef __wasm__
    const char* executable = "/bin/check";
#else
    const char* executable = argv[0];
#endif
    for (unsigned variant = 0; variant < 3; ++variant) {
        pid_t child = vfork();
        assert(child >= 0);
        if (child == 0) {
            if (variant == 1) {
                execv(executable, arguments);
                _exit(127);
            }
            if (variant == 2) {
                execv("/missing-vfork-executable", arguments);
                _exit(errno == ENOENT ? 31 : 127);
            }
            _exit(17);
        }
        int status;
        assert(wait(&status) == child && WIFEXITED(status));
        if (WEXITSTATUS(status) != (variant == 0 ? 17 : variant == 1 ? 23 : 31)) {
            fprintf(stderr, "vfork variant %u: status %d\n", variant, status);
        }
        assert(WEXITSTATUS(status) == (variant == 0 ? 17 : variant == 1 ? 23 : 31));
        assert(waitpid(child, &status, WNOHANG) == -1 && errno == ECHILD);
    }
    puts("vfork execution: ok");
}
