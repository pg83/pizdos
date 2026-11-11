//@ compile-flags: -g -D_GNU_SOURCE
//@ stdout: "vectored failures: ok\n"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <sys/uio.h>
#include <unistd.h>

static volatile sig_atomic_t broken;

static void pipeBroken(int signal) {
    assert(signal == SIGPIPE);
    broken = broken + 1;
}

int main() {
    char bytes[] = "abc";
    iovec data[] = {{bytes, 1}, {bytes + 1, 2}};
    int full = open("/dev/full", O_WRONLY);
    assert(full >= 0);
    assert(writev(full, data, 2) == -1 && errno == ENOSPC);
    assert(pwritev(full, data, 2, 0) == -1 && errno == ENOSPC);
    assert(readv(full, data, 2) == -1 && errno == EBADF);
    assert(close(full) == 0);
    assert(signal(SIGPIPE, pipeBroken) != SIG_ERR);
    int ends[2];
    assert(pipe(ends) == 0 && close(ends[0]) == 0);
    assert(writev(ends[1], data, 2) == -1 && errno == EPIPE && broken == 1);
    assert(write(ends[1], bytes, sizeof(bytes)) == -1 && errno == EPIPE && broken == 2);
    assert(close(ends[1]) == 0);
    puts("vectored failures: ok");
}
