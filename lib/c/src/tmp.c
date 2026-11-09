#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * Temporary files and names in P_tmpdir, made by the kernel's mkstemp and
 * mktemp: a name of tmpnam or tempnam was free when made, as POSIX allows.
 */

FILE* tmpfile(void) {
    char path[] = P_tmpdir "/tmpfile_XXXXXX";
    int fd = mkstemp(path);

    if (fd < 0) {
        return 0;
    }

    unlink(path);

    FILE* f = fdopen(fd, "w+");

    if (!f) {
        close(fd);
    }

    return f;
}

char* tmpnam(char* buf) {
    static char internal[L_tmpnam];
    char path[] = P_tmpdir "/tmpnam_XXXXXX";

    if (!*mktemp(path)) {
        return 0;
    }

    return strcpy(buf ? buf : internal, path);
}

char* tempnam(const char* dir, const char* prefix) {
    if (!dir) {
        dir = P_tmpdir;
    }

    if (!prefix) {
        prefix = "temp";
    }

    size_t dirLength = strlen(dir);
    size_t prefixLength = strlen(prefix);
    char* path = malloc(dirLength + prefixLength + 9);

    if (!path) {
        return 0;
    }

    memcpy(path, dir, dirLength);
    path[dirLength] = '/';
    memcpy(path + dirLength + 1, prefix, prefixLength);
    memcpy(path + dirLength + 1 + prefixLength, "_XXXXXX", 8);

    if (!*mktemp(path)) {
        free(path);

        return 0;
    }

    return path;
}
