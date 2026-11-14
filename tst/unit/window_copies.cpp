//@ host: false
//@ compile-flags: -O2 -D_GNU_SOURCE
//@ stdout: "window copies: ok\n"

// copy_file_range between a memfd and a file of the host: the memfd's
// bytes are the memory it is of, so the file's bytes go straight there and
// come straight from there, the host reading and writing that memory. An
// offset given moves, else the file's position does; the memfd's range
// does not grow, its grant is kept, and a file that is no regular one, the
// memfd itself as both ends, or flags are refused. A memfd into another
// memfd moves the bytes.

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

int main() {
    char window[32];
    memset(window, '.', sizeof window);
    const int w = memfd_create(window, sizeof window, O_RDWR);
    const int f = open("f", O_RDWR | O_CREAT | O_TRUNC, 0644);
    assert(w >= 0 && f >= 0);
    assert(write(f, "0123456789abcdef", 16) == 16 && lseek(f, 0, SEEK_SET) == 0);

    // The file's bytes into the window, at offsets given, which move.
    off_t from = 2;
    off_t to = 5;
    assert(copy_file_range(f, &from, w, &to, 4, 0) == 4 && from == 6 && to == 9);
    assert(memcmp(window, ".....2345...", 12) == 0);

    // At the positions, which move.
    assert(lseek(w, 20, SEEK_SET) == 20);
    assert(copy_file_range(f, nullptr, w, nullptr, 3, 0) == 3);
    assert(lseek(f, 0, SEEK_CUR) == 3 && lseek(w, 0, SEEK_CUR) == 23);
    assert(memcmp(window + 18, "..012.", 6) == 0);

    // The window's range ends: what is past it does not go, and nothing
    // goes from past its end; at its end the copy is ENOSPC.
    to = 30;
    from = 0;
    assert(copy_file_range(f, &from, w, &to, 8, 0) == 2 && to == 32 && memcmp(window + 30, "01", 2) == 0);
    assert(copy_file_range(f, &from, w, &to, 1, 0) == -1 && errno == ENOSPC);
    from = 32;
    to = 0;
    assert(copy_file_range(w, &from, f, &to, 4, 0) == 0);

    // A window's bytes into the file.
    char letters[3] = {'X', 'Y', 'Z'};
    const int r = memfd_create(letters, sizeof letters, O_RDONLY);
    to = 10;
    assert(r >= 0 && copy_file_range(r, nullptr, f, &to, 8, 0) == 3 && to == 13);
    char content[17] = {};
    assert(pread(f, content, 16, 0) == 16 && strcmp(content, "0123456789XYZdef") == 0);

    // A grant is kept: what reads only is no destination, what writes only
    // no source.
    to = 0;
    assert(copy_file_range(f, nullptr, r, &to, 1, 0) == -1 && errno == EBADF);
    const int wo = memfd_create(window, sizeof window, O_WRONLY);
    from = 0;
    assert(copy_file_range(wo, &from, f, nullptr, 1, 0) == -1 && errno == EBADF);

    // A window into another moves the bytes.
    char other[8] = {};
    const int o = memfd_create(other, sizeof other, O_RDWR);
    from = 5;
    to = 2;
    assert(copy_file_range(w, &from, o, &to, 4, 0) == 4 && memcmp(other + 2, "2345", 4) == 0 && from == 9 && to == 6);

    // No pipe, no window as both ends, no flags.
    int pipes[2];
    assert(pipe(pipes) == 0);
    assert(copy_file_range(pipes[0], nullptr, w, nullptr, 1, 0) == -1 && errno == EINVAL);
    from = 0;
    to = 8;
    assert(copy_file_range(w, &from, w, &to, 1, 0) == -1 && errno == EINVAL);
    assert(copy_file_range(f, &from, w, &to, 1, 1) == -1 && errno == EINVAL);

    printf("window copies: ok\n");
    return 0;
}
