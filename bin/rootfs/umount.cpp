// umount: unmounts what is mounted on where (umount(2), sys/mount.h).
//
//     umount <where>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <unistd.h>

int main(int argc, char** argv) {
    if (argc != 2) {
        dprintf(STDERR_FILENO, "usage: umount <where>\n");
        return 2;
    }
    if (umount(argv[1]) != 0) {
        dprintf(STDERR_FILENO, "umount: %s: %s\n", argv[1], strerror(errno));
        return 1;
    }
    return 0;
}
