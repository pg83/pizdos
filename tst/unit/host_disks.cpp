//@ host: false
//@ run-flags: --disk first=$ROOT/bin/check --disk second=$ROOT/bin/check
//@ stdout: "first second PZD1 PZD1 read-write\n"

// vm run --disk gives the first process files of the host as disks: its
// descriptors after 0, 1 and 2, in order, open to read and write, and
// their names in DISKS. The disks here are the program's own executable.

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main() {
    char first[5] = {};
    char second[5] = {};
    const bool read = pread(3, first, 4, 0) == 4 && pread(4, second, 4, 0) == 4;
    const bool both = (fcntl(3, F_GETFL) & O_ACCMODE) == O_RDWR && (fcntl(4, F_GETFL) & O_ACCMODE) == O_RDWR;
    printf("%s %s %s %s\n", getenv("DISKS"), read ? first : "?", second, both ? "read-write" : "?");
    return 0;
}
