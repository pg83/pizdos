// mount: mounts what, a directory of a server, a file system's service of
// /dev say, on where, a directory: a walk into where goes on to that
// server from then on (mount(2), sys/mount.h).
//
//     mount <where> <what>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <unistd.h>

int main(int argc, char** argv) {
    if (argc != 3) {
        dprintf(STDERR_FILENO, "usage: mount <where> <what>\n");
        return 2;
    }
    if (mount(argv[2], argv[1], nullptr, 0, nullptr) != 0) {
        dprintf(STDERR_FILENO, "mount: %s on %s: %s\n", argv[2], argv[1], strerror(errno));
        return 1;
    }
    return 0;
}
