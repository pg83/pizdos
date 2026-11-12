#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static int descriptors[128];

// A pipe is the kernel's and takes no host descriptor: it fails only when
// the process's own table, of three descriptors and those made so far, is
// full.
static void checkPipe(int count) {
    struct rlimit limit;
    int ends[2] = {-1, -1};
    assert(getrlimit(RLIMIT_NOFILE, &limit) == 0);
    if ((rlim_t)count + 3 + 2 <= limit.rlim_cur) {
        assert(pipe2(ends, O_CLOEXEC) == 0 && close(ends[0]) == 0 && close(ends[1]) == 0);
    } else {
        assert(pipe2(ends, O_CLOEXEC) == -1 && errno == EMFILE);
        assert(ends[0] == -1 && ends[1] == -1);
    }
}

int main(int argc, char** argv) {
    assert(argc == 2);
    int count = 0;
    while (count < 128 && (descriptors[count] = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0)) >= 0) {
        ++count;
    }
    assert(count > 4 && count < 128 && errno == EMFILE);
    int ends[2] = {-1, -1}, master = -1, slave = -1;
    if (strcmp(argv[1], "objects") == 0) {
        checkPipe(count);
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, ends) == -1 && errno == EMFILE);
        assert(posix_openpt(O_RDWR | O_NOCTTY) == -1 && errno == EMFILE);
        assert(openpty(&master, &slave, NULL, NULL, NULL) == -1 && errno == EMFILE);
        assert(forkpty(&master, NULL, NULL, NULL) == -1 && errno == EMFILE);
        assert(close(descriptors[--count]) == 0);
        for (int pass = 0; pass != 3; ++pass) {
            // A master can open, but its slave needs one more descriptor.
            assert(openpty(&master, &slave, NULL, NULL, NULL) == -1 && errno == EMFILE);
            checkPipe(count);
            assert(socketpair(AF_UNIX, SOCK_STREAM, 0, ends) == -1 && errno == EMFILE);
        }
        int fd = open("/dev/null", O_RDONLY);
        assert(fd >= 0 && close(fd) == 0);
        assert(close(descriptors[--count]) == 0);
        assert(pipe(ends) == 0 && close(ends[0]) == 0 && close(ends[1]) == 0);
        assert(openpty(&master, &slave, NULL, NULL, NULL) == 0);
        assert(close(master) == 0 && close(slave) == 0);
    } else {
        pid_t child;
        if (strcmp(argv[1], "forkpty") == 0) {
            assert(close(descriptors[--count]) == 0 && close(descriptors[--count]) == 0);
            child = forkpty(&master, NULL, NULL, NULL);
        } else if (strcmp(argv[1], "vfork") == 0) {
            child = vfork();
        } else {
            assert(strcmp(argv[1], "fork") == 0);
            child = fork();
        }
        if (child == 0) {
            _exit(17);
        }
        if (child < 0) {
            assert(errno == EAGAIN || errno == EMFILE || errno == ENOMEM);
        } else {
            int status;
            assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 17);
            if (strcmp(argv[1], "forkpty") == 0) {
                assert(close(master) == 0);
            }
        }
        if (strcmp(argv[1], "forkpty") == 0) {
            assert(pipe(ends) == 0 && close(ends[0]) == 0 && close(ends[1]) == 0);
        }
    }
    while (count) {
        assert(close(descriptors[--count]) == 0);
    }
    assert(pipe(ends) == 0 && close(ends[0]) == 0 && close(ends[1]) == 0);
    puts("host resource recovery: ok");
    return 0;
}
