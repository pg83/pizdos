//@ compile-flags: -g -D_GNU_SOURCE
//@ stdout: "path globs: ok\n"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int errors;
static bool stop;

static int inaccessible(const char* path, int error) {
    assert(strncmp(path, "loop", 4) == 0 && error == ELOOP);
    errors++;
    return stop;
}

static void touch(const char* name) {
    int fd = open(name, O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0 && close(fd) == 0);
}

int main() {
    assert(mkdir("glob-case", 0700) == 0 && chdir("glob-case") == 0);
    touch("a.txt");
    touch("b.txt");
    touch(".hidden");
    touch("star*");
    touch("slash\\name");
    assert(mkdir("sub", 0700) == 0);
    touch("sub/x.txt");
    assert(symlink("loop", "loop") == 0);
    glob_t paths{};
    assert(glob("*.txt", 0, nullptr, &paths) == 0 && paths.gl_pathc == 2);
    assert(strcmp(paths.gl_pathv[0], "a.txt") == 0 && strcmp(paths.gl_pathv[1], "b.txt") == 0 && paths.gl_pathv[2] == nullptr);
    globfree(&paths);
    paths = {};
    paths.gl_offs = 2;
    assert(glob("a.txt", GLOB_DOOFFS, nullptr, &paths) == 0 && paths.gl_pathc == 1);
    assert(paths.gl_pathv[0] == nullptr && paths.gl_pathv[1] == nullptr && strcmp(paths.gl_pathv[2], "a.txt") == 0);
    assert(glob("absent", GLOB_APPEND | GLOB_DOOFFS | GLOB_NOCHECK, nullptr, &paths) == 0 && paths.gl_pathc == 2);
    assert(strcmp(paths.gl_pathv[2], "a.txt") == 0 && strcmp(paths.gl_pathv[3], "absent") == 0 && paths.gl_pathv[4] == nullptr);
    globfree(&paths);
    const char* patterns[] = {"sub/", "sub//*.txt", "star\\*", "slash\\*", ".*", "*.txt", ".h*"};
    const int flags[] = {0, 0, 0, GLOB_NOESCAPE, 0, GLOB_NOSORT, GLOB_PERIOD};
    const size_t counts[] = {1, 1, 1, 1, 3, 2, 1};
    const char* first[] = {"sub/", "sub//x.txt", "star*", "slash\\name", ".", nullptr, ".hidden"};
    for (unsigned i = 0; i < sizeof(patterns) / sizeof(patterns[0]); ++i) {
        paths = {};
        assert(glob(patterns[i], flags[i], nullptr, &paths) == 0 && paths.gl_pathc == counts[i]);
        if (first[i]) {
            if (strcmp(paths.gl_pathv[0], first[i]) != 0) {
                fprintf(stderr, "glob('%s'): got '%s', expected '%s'\n", patterns[i], paths.gl_pathv[0], first[i]);
                abort();
            }
        }
        globfree(&paths);
    }
    paths = {};
    assert(glob("s*", GLOB_MARK, nullptr, &paths) == 0 && paths.gl_pathc == 3);
    assert(strcmp(paths.gl_pathv[0], "slash\\name") == 0 && strcmp(paths.gl_pathv[1], "star*") == 0 && strcmp(paths.gl_pathv[2], "sub/") == 0);
    globfree(&paths);
    paths = {};
    assert(glob("absent*", 0, nullptr, &paths) == GLOB_NOMATCH);
    globfree(&paths);
    paths = {};
    assert(glob("", 0, nullptr, &paths) == GLOB_NOMATCH);
    globfree(&paths);
    paths = {};
    assert(glob("loop/*", 0, inaccessible, &paths) == GLOB_NOMATCH && errors == 1);
    globfree(&paths);
    stop = true;
    paths = {};
    assert(glob("loop/*", 0, inaccessible, &paths) == GLOB_ABORTED && errors == 2);
    globfree(&paths);
    paths = {};
    assert(glob("loop/*", GLOB_ERR, nullptr, &paths) == GLOB_ABORTED);
    globfree(&paths);
    char home[1024], expected[1050];
    assert(getcwd(home, sizeof(home)) == home && setenv("HOME", home, 1) == 0);
    snprintf(expected, sizeof(expected), "%s/a.txt", home);
    paths = {};
    assert(glob("~/a.txt", GLOB_TILDE, nullptr, &paths) == 0 && paths.gl_pathc == 1);
    assert(strcmp(paths.gl_pathv[0], expected) == 0);
    globfree(&paths);
    paths = {};
    assert(glob("~pizdos-nonexistent-user/a", GLOB_TILDE_CHECK, nullptr, &paths) == GLOB_NOMATCH);
    globfree(&paths);
#ifdef __wasm__
    // musl rejects unknown users even without GLOB_TILDE_CHECK.
    paths = {};
    assert(glob("~pizdos-nonexistent-user/a", GLOB_TILDE | GLOB_NOCHECK, nullptr, &paths) == 0);
    assert(strcmp(paths.gl_pathv[0], "~pizdos-nonexistent-user/a") == 0);
    globfree(&paths);
#endif
    paths = {};
    assert(glob("~", GLOB_TILDE, nullptr, &paths) == 0 && paths.gl_pathc == 1);
    assert(strcmp(paths.gl_pathv[0], home) == 0);
    globfree(&paths);
    puts("path globs: ok");
}
