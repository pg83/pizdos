// WASI preview1 over POSIX, enough for Go's wasip1 port. The root directory
// is preopened as fd 3; PWD is the host's working directory, so relative
// paths resolve as on the host.
#define _GNU_SOURCE

#include "wasi.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

extern char** environ;

static int args_count;
static char** args;
static char** envs;
static int envs_count;

enum {
    ok = 0,
    e2big = 1,
    eacces = 2,
    ebadf = 8,
    eexist = 20,
    efault = 21,
    einval = 28,
    eio = 29,
    eisdir = 31,
    eloop = 32,
    enametoolong = 37,
    enoent = 44,
    enomem = 48,
    enospc = 51,
    enosys = 52,
    enotdir = 54,
    enotempty = 55,
    eperm = 63,
    espipe = 70,
};

static uint32_t error(int e) {
    switch (e) {
    case E2BIG:
        return e2big;
    case EACCES:
        return eacces;
    case EBADF:
        return ebadf;
    case EEXIST:
        return eexist;
    case EFAULT:
        return efault;
    case EINVAL:
        return einval;
    case EISDIR:
        return eisdir;
    case ELOOP:
        return eloop;
    case ENAMETOOLONG:
        return enametoolong;
    case ENOENT:
        return enoent;
    case ENOMEM:
        return enomem;
    case ENOSPC:
        return enospc;
    case ENOSYS:
        return enosys;
    case ENOTDIR:
        return enotdir;
    case ENOTEMPTY:
        return enotempty;
    case EPERM:
        return eperm;
    case ESPIPE:
        return espipe;
    }

    return eio;
}

#define CHECK(m, n, at, size)                                  \
    do {                                                       \
        if ((uint64_t)(at) + (uint64_t)(size) > (n)) {         \
            return efault;                                     \
        }                                                      \
    } while (0)

static void put32(uint8_t* m, uint32_t at, uint32_t v) {
    memcpy(m + at, &v, 4);
}

static void put64(uint8_t* m, uint32_t at, uint64_t v) {
    memcpy(m + at, &v, 8);
}

static uint32_t get32(uint8_t* m, uint32_t at) {
    uint32_t v;
    memcpy(&v, m + at, 4);
    return v;
}

static uint64_t get64(uint8_t* m, uint32_t at) {
    uint64_t v;
    memcpy(&v, m + at, 8);
    return v;
}

static int root = -1;

void wasi_init(int argc, char** argv) {
    args_count = argc;
    args = argv;
    root = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);

    if (root < 0 || root == 3) {
        // fd 3 is the preopen: keep it free for it
    }

    if (root != 3) {
        if (dup2(root, 3) < 0) {
            perror("dup2");
            exit(1);
        }

        close(root);
        root = 3;
    }

    int count = 0;

    while (environ[count]) {
        count++;
    }

    envs = calloc(count + 2, sizeof(char*));
    char cwd[4096];

    if (!getcwd(cwd, sizeof(cwd))) {
        perror("getcwd");
        exit(1);
    }

    char* pwd = malloc(strlen(cwd) + 5);
    strcpy(pwd, "PWD=");
    strcat(pwd, cwd);
    envs[envs_count++] = pwd;

    for (int i = 0; i < count; i++) {
        if (strncmp(environ[i], "PWD=", 4) != 0) {
            envs[envs_count++] = environ[i];
        }
    }
}

static uint32_t strings_sizes(uint8_t* m, uint64_t n, char** list, int count, uint32_t pcount, uint32_t psize) {
    CHECK(m, n, pcount, 4);
    CHECK(m, n, psize, 4);
    uint32_t size = 0;

    for (int i = 0; i < count; i++) {
        size += strlen(list[i]) + 1;
    }

    put32(m, pcount, count);
    put32(m, psize, size);
    return ok;
}

static uint32_t strings_get(uint8_t* m, uint64_t n, char** list, int count, uint32_t ptrs, uint32_t buf) {
    CHECK(m, n, ptrs, 4 * (uint64_t)count);

    for (int i = 0; i < count; i++) {
        size_t len = strlen(list[i]) + 1;
        CHECK(m, n, buf, len);
        put32(m, ptrs + 4 * i, buf);
        memcpy(m + buf, list[i], len);
        buf += len;
    }

    return ok;
}

uint32_t wasi_args_get(uint8_t* m, uint64_t n, uint32_t argv, uint32_t buf) {
    return strings_get(m, n, args, args_count, argv, buf);
}

uint32_t wasi_args_sizes_get(uint8_t* m, uint64_t n, uint32_t argc, uint32_t size) {
    return strings_sizes(m, n, args, args_count, argc, size);
}

uint32_t wasi_environ_get(uint8_t* m, uint64_t n, uint32_t env, uint32_t buf) {
    return strings_get(m, n, envs, envs_count, env, buf);
}

uint32_t wasi_environ_sizes_get(uint8_t* m, uint64_t n, uint32_t count, uint32_t size) {
    return strings_sizes(m, n, envs, envs_count, count, size);
}

static clockid_t clock_of(uint32_t id) {
    switch (id) {
    case 0:
        return CLOCK_REALTIME;
    case 1:
        return CLOCK_MONOTONIC;
    case 2:
        return CLOCK_PROCESS_CPUTIME_ID;
    }

    return CLOCK_THREAD_CPUTIME_ID;
}

uint32_t wasi_clock_time_get(uint8_t* m, uint64_t n, uint32_t id, uint64_t precision, uint32_t time) {
    CHECK(m, n, time, 8);
    struct timespec ts;

    if (id > 3) {
        return einval;
    }

    if (clock_gettime(clock_of(id), &ts) != 0) {
        return error(errno);
    }

    put64(m, time, (uint64_t)ts.tv_sec * 1000000000u + ts.tv_nsec);
    return ok;
}

uint32_t wasi_clock_res_get(uint8_t* m, uint64_t n, uint32_t id, uint32_t res) {
    CHECK(m, n, res, 8);
    struct timespec ts;

    if (id > 3) {
        return einval;
    }

    if (clock_getres(clock_of(id), &ts) != 0) {
        return error(errno);
    }

    put64(m, res, (uint64_t)ts.tv_sec * 1000000000u + ts.tv_nsec);
    return ok;
}

// iovecs collects a guest iovec array.
static uint32_t iovecs(uint8_t* m, uint64_t n, uint32_t iovs, uint32_t count, struct iovec* out) {
    if (count > 1024) {
        return einval;
    }

    CHECK(m, n, iovs, 8 * (uint64_t)count);

    for (uint32_t i = 0; i < count; i++) {
        uint32_t base = get32(m, iovs + 8 * i);
        uint32_t len = get32(m, iovs + 8 * i + 4);
        CHECK(m, n, base, len);
        out[i].iov_base = m + base;
        out[i].iov_len = len;
    }

    return ok;
}

uint32_t wasi_fd_write(uint8_t* m, uint64_t n, uint32_t fd, uint32_t iovs, uint32_t count, uint32_t written) {
    struct iovec v[1024];
    uint32_t e = iovecs(m, n, iovs, count, v);

    if (e != ok) {
        return e;
    }

    CHECK(m, n, written, 4);
    ssize_t r = writev(fd, v, count);

    if (r < 0) {
        return error(errno);
    }

    put32(m, written, r);
    return ok;
}

uint32_t wasi_fd_read(uint8_t* m, uint64_t n, uint32_t fd, uint32_t iovs, uint32_t count, uint32_t read) {
    struct iovec v[1024];
    uint32_t e = iovecs(m, n, iovs, count, v);

    if (e != ok) {
        return e;
    }

    CHECK(m, n, read, 4);
    ssize_t r = readv(fd, v, count);

    if (r < 0) {
        return error(errno);
    }

    put32(m, read, r);
    return ok;
}

uint32_t wasi_fd_pwrite(uint8_t* m, uint64_t n, uint32_t fd, uint32_t iovs, uint32_t count, uint64_t offset, uint32_t written) {
    struct iovec v[1024];
    uint32_t e = iovecs(m, n, iovs, count, v);

    if (e != ok) {
        return e;
    }

    CHECK(m, n, written, 4);
    ssize_t r = pwritev(fd, v, count, offset);

    if (r < 0) {
        return error(errno);
    }

    put32(m, written, r);
    return ok;
}

uint32_t wasi_fd_pread(uint8_t* m, uint64_t n, uint32_t fd, uint32_t iovs, uint32_t count, uint64_t offset, uint32_t read) {
    struct iovec v[1024];
    uint32_t e = iovecs(m, n, iovs, count, v);

    if (e != ok) {
        return e;
    }

    CHECK(m, n, read, 4);
    ssize_t r = preadv(fd, v, count, offset);

    if (r < 0) {
        return error(errno);
    }

    put32(m, read, r);
    return ok;
}

uint32_t wasi_fd_close(uint8_t* m, uint64_t n, uint32_t fd) {
    if (fd == 3) {
        return ok;
    }

    return close(fd) == 0 ? ok : error(errno);
}

uint32_t wasi_fd_seek(uint8_t* m, uint64_t n, uint32_t fd, uint64_t offset, uint32_t whence, uint32_t result) {
    CHECK(m, n, result, 8);
    int w = whence == 0 ? SEEK_SET : whence == 1 ? SEEK_CUR : SEEK_END;
    off_t r = lseek(fd, (off_t)offset, w);

    if (r < 0) {
        return error(errno);
    }

    put64(m, result, r);
    return ok;
}

uint32_t wasi_fd_sync(uint8_t* m, uint64_t n, uint32_t fd) {
    return fsync(fd) == 0 ? ok : error(errno);
}

static uint8_t filetype(mode_t mode) {
    switch (mode & S_IFMT) {
    case S_IFBLK:
        return 1;
    case S_IFCHR:
        return 2;
    case S_IFDIR:
        return 3;
    case S_IFREG:
        return 4;
    case S_IFSOCK:
        return 6;
    case S_IFLNK:
        return 7;
    }

    return 0;
}

uint32_t wasi_fd_fdstat_get(uint8_t* m, uint64_t n, uint32_t fd, uint32_t stat) {
    CHECK(m, n, stat, 24);
    struct stat st;

    if (fstat(fd, &st) != 0) {
        return error(errno);
    }

    int fl = fcntl(fd, F_GETFL);
    uint16_t flags = 0;

    if (fl & O_APPEND) {
        flags |= 1;
    }

    if (fl & O_NONBLOCK) {
        flags |= 4;
    }

    memset(m + stat, 0, 24);
    m[stat] = filetype(st.st_mode);
    memcpy(m + stat + 2, &flags, 2);
    put64(m, stat + 8, ~(uint64_t)0);
    put64(m, stat + 16, ~(uint64_t)0);
    return ok;
}

uint32_t wasi_fd_fdstat_set_flags(uint8_t* m, uint64_t n, uint32_t fd, uint32_t flags) {
    int fl = fcntl(fd, F_GETFL);

    if (fl < 0) {
        return error(errno);
    }

    fl &= ~(O_APPEND | O_NONBLOCK);

    if (flags & 1) {
        fl |= O_APPEND;
    }

    if (flags & 4) {
        fl |= O_NONBLOCK;
    }

    return fcntl(fd, F_SETFL, fl) == 0 ? ok : error(errno);
}

static void filestat(uint8_t* m, uint32_t at, struct stat* st) {
    memset(m + at, 0, 64);
    put64(m, at, st->st_dev);
    put64(m, at + 8, st->st_ino);
    m[at + 16] = filetype(st->st_mode);
    put64(m, at + 24, st->st_nlink);
    put64(m, at + 32, st->st_size);
    put64(m, at + 40, (uint64_t)st->st_atim.tv_sec * 1000000000u + st->st_atim.tv_nsec);
    put64(m, at + 48, (uint64_t)st->st_mtim.tv_sec * 1000000000u + st->st_mtim.tv_nsec);
    put64(m, at + 56, (uint64_t)st->st_ctim.tv_sec * 1000000000u + st->st_ctim.tv_nsec);
}

uint32_t wasi_fd_filestat_get(uint8_t* m, uint64_t n, uint32_t fd, uint32_t stat) {
    CHECK(m, n, stat, 64);
    struct stat st;

    if (fstat(fd, &st) != 0) {
        return error(errno);
    }

    filestat(m, stat, &st);
    return ok;
}

uint32_t wasi_fd_filestat_set_size(uint8_t* m, uint64_t n, uint32_t fd, uint64_t size) {
    return ftruncate(fd, (off_t)size) == 0 ? ok : error(errno);
}

uint32_t wasi_fd_prestat_get(uint8_t* m, uint64_t n, uint32_t fd, uint32_t stat) {
    CHECK(m, n, stat, 8);

    if (fd != 3) {
        return ebadf;
    }

    put32(m, stat, 0);
    put32(m, stat + 4, 1);
    return ok;
}

uint32_t wasi_fd_prestat_dir_name(uint8_t* m, uint64_t n, uint32_t fd, uint32_t path, uint32_t len) {
    if (fd != 3) {
        return ebadf;
    }

    CHECK(m, n, path, len);

    if (len < 1) {
        return einval;
    }

    m[path] = '/';
    return ok;
}

uint32_t wasi_fd_readdir(uint8_t* m, uint64_t n, uint32_t fd, uint32_t buf, uint32_t len, uint64_t cookie, uint32_t used) {
    CHECK(m, n, buf, len);
    CHECK(m, n, used, 4);
    int dfd = dup(fd);

    if (dfd < 0) {
        return error(errno);
    }

    DIR* d = fdopendir(dfd);

    if (!d) {
        close(dfd);
        return error(errno);
    }

    rewinddir(d);
    uint64_t index = 0;
    uint32_t out = 0;
    struct dirent* ent;

    while ((ent = readdir(d)) != NULL) {
        if (index++ < cookie) {
            continue;
        }

        size_t name = strlen(ent->d_name);
        uint8_t head[24];
        uint64_t next = index;
        uint64_t ino = ent->d_ino;
        uint32_t namelen = name;
        memcpy(head, &next, 8);
        memcpy(head + 8, &ino, 8);
        memcpy(head + 16, &namelen, 4);
        uint8_t type = 0;

        switch (ent->d_type) {
        case DT_BLK:
            type = 1;
            break;
        case DT_CHR:
            type = 2;
            break;
        case DT_DIR:
            type = 3;
            break;
        case DT_REG:
            type = 4;
            break;
        case DT_LNK:
            type = 7;
            break;
        }

        memset(head + 20, 0, 4);
        head[20] = type;

        for (size_t i = 0; i < 24 && out < len; i++) {
            m[buf + out++] = head[i];
        }

        for (size_t i = 0; i < name && out < len; i++) {
            m[buf + out++] = ent->d_name[i];
        }

        if (out >= len) {
            break;
        }
    }

    closedir(d);
    put32(m, used, out);
    return ok;
}

// guest_path copies a path to a NUL-terminated string; absolute guest paths
// are relative to the preopened root.
static uint32_t guest_path(uint8_t* m, uint64_t n, uint32_t path, uint32_t len, char* out) {
    CHECK(m, n, path, len);

    if (len >= 4096) {
        return enametoolong;
    }

    memcpy(out, m + path, len);
    out[len] = 0;

    if (len == 0) {
        strcpy(out, ".");
    }

    return ok;
}

uint32_t wasi_path_filestat_get(uint8_t* m, uint64_t n, uint32_t fd, uint32_t flags, uint32_t path, uint32_t len, uint32_t stat) {
    char p[4096];
    uint32_t e = guest_path(m, n, path, len, p);

    if (e != ok) {
        return e;
    }

    CHECK(m, n, stat, 64);
    struct stat st;

    if (fstatat(fd, p, &st, (flags & 1) ? 0 : AT_SYMLINK_NOFOLLOW) != 0) {
        return error(errno);
    }

    filestat(m, stat, &st);
    return ok;
}

uint32_t wasi_path_open(uint8_t* m, uint64_t n, uint32_t fd, uint32_t dirflags, uint32_t path, uint32_t len, uint32_t oflags,
                        uint64_t rights, uint64_t inheriting, uint32_t fdflags, uint32_t opened) {
    char p[4096];
    uint32_t e = guest_path(m, n, path, len, p);

    if (e != ok) {
        return e;
    }

    CHECK(m, n, opened, 4);
    int read = (rights & (1 << 1)) != 0 || (rights & (1 << 14)) != 0;
    int write = (rights & (1 << 6)) != 0 || (rights & (1 << 22)) != 0;
    int flags = O_CLOEXEC;

    if (read && write) {
        flags |= O_RDWR;
    } else if (write) {
        flags |= O_WRONLY;
    } else {
        flags |= O_RDONLY;
    }

    if (oflags & 1) {
        flags |= O_CREAT;
    }

    if (oflags & 2) {
        flags |= O_DIRECTORY;
    }

    if (oflags & 4) {
        flags |= O_EXCL;
    }

    if (oflags & 8) {
        flags |= O_TRUNC;
    }

    if (fdflags & 1) {
        flags |= O_APPEND;
    }

    if (fdflags & 4) {
        flags |= O_NONBLOCK;
    }

    if (!(dirflags & 1)) {
        flags |= O_NOFOLLOW;
    }

    int r = openat(fd, p, flags, 0666);

    if (r < 0 && errno == EISDIR && write && !(oflags & 1)) {
        r = openat(fd, p, (flags & ~(O_WRONLY | O_RDWR)) | O_RDONLY, 0666);
    }

    if (r < 0) {
        return error(errno);
    }

    put32(m, opened, r);
    return ok;
}

uint32_t wasi_path_create_directory(uint8_t* m, uint64_t n, uint32_t fd, uint32_t path, uint32_t len) {
    char p[4096];
    uint32_t e = guest_path(m, n, path, len, p);

    if (e != ok) {
        return e;
    }

    return mkdirat(fd, p, 0777) == 0 ? ok : error(errno);
}

uint32_t wasi_path_remove_directory(uint8_t* m, uint64_t n, uint32_t fd, uint32_t path, uint32_t len) {
    char p[4096];
    uint32_t e = guest_path(m, n, path, len, p);

    if (e != ok) {
        return e;
    }

    return unlinkat(fd, p, AT_REMOVEDIR) == 0 ? ok : error(errno);
}

uint32_t wasi_path_unlink_file(uint8_t* m, uint64_t n, uint32_t fd, uint32_t path, uint32_t len) {
    char p[4096];
    uint32_t e = guest_path(m, n, path, len, p);

    if (e != ok) {
        return e;
    }

    return unlinkat(fd, p, 0) == 0 ? ok : error(errno);
}

uint32_t wasi_path_rename(uint8_t* m, uint64_t n, uint32_t fd, uint32_t path, uint32_t len, uint32_t fd2, uint32_t path2, uint32_t len2) {
    char p[4096], q[4096];
    uint32_t e = guest_path(m, n, path, len, p);

    if (e == ok) {
        e = guest_path(m, n, path2, len2, q);
    }

    if (e != ok) {
        return e;
    }

    return renameat(fd, p, fd2, q) == 0 ? ok : error(errno);
}

uint32_t wasi_path_readlink(uint8_t* m, uint64_t n, uint32_t fd, uint32_t path, uint32_t len, uint32_t buf, uint32_t buflen, uint32_t used) {
    char p[4096];
    uint32_t e = guest_path(m, n, path, len, p);

    if (e != ok) {
        return e;
    }

    CHECK(m, n, buf, buflen);
    CHECK(m, n, used, 4);
    ssize_t r = readlinkat(fd, p, (char*)m + buf, buflen);

    if (r < 0) {
        return error(errno);
    }

    put32(m, used, r);
    return ok;
}

// poll_oneoff sleeps until the earliest clock subscription; fd
// subscriptions are reported ready at once.
uint32_t wasi_poll_oneoff(uint8_t* m, uint64_t n, uint32_t in, uint32_t out, uint32_t count, uint32_t events) {
    if (count == 0) {
        return einval;
    }

    CHECK(m, n, in, 48 * (uint64_t)count);
    CHECK(m, n, out, 32 * (uint64_t)count);
    CHECK(m, n, events, 4);
    uint32_t ready = 0;
    int64_t sleep = -1;
    uint32_t clock = 0;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t s = in + 48 * i;
        uint8_t tag = m[s + 8];

        if (tag != 0) {
            uint32_t e = out + 32 * ready++;
            memset(m + e, 0, 32);
            put64(m, e, get64(m, s));
            m[e + 10] = tag;
            continue;
        }

        uint64_t timeout = get64(m, s + 24);
        uint16_t flags;
        memcpy(&flags, m + s + 40, 2);

        if (flags & 1) {
            struct timespec now;
            clock_gettime(clock_of(get32(m, s + 16)), &now);
            uint64_t t = (uint64_t)now.tv_sec * 1000000000u + now.tv_nsec;
            timeout = timeout > t ? timeout - t : 0;
        }

        if (sleep < 0 || (int64_t)timeout < sleep) {
            sleep = timeout;
            clock = i;
        }
    }

    if (ready == 0 && sleep >= 0) {
        struct timespec ts = {sleep / 1000000000, sleep % 1000000000};
        nanosleep(&ts, NULL);
        uint32_t e = out;
        memset(m + e, 0, 32);
        put64(m, e, get64(m, in + 48 * clock));
        ready = 1;
    }

    put32(m, events, ready);
    return ok;
}

uint32_t wasi_random_get(uint8_t* m, uint64_t n, uint32_t buf, uint32_t len) {
    CHECK(m, n, buf, len);
    uint32_t done = 0;

    while (done < len) {
        ssize_t r = getrandom(m + buf + done, len - done, 0);

        if (r < 0) {
            return error(errno);
        }

        done += r;
    }

    return ok;
}

uint32_t wasi_sched_yield(uint8_t* m, uint64_t n) {
    return ok;
}

void wasi_proc_exit(uint8_t* m, uint64_t n, uint32_t code) {
    _exit(code);
}

struct run {
    int (*fn)(void*);
    void* arg;
    int result;
};

static void* trampoline(void* p) {
    struct run* r = p;
    r->result = r->fn(r->arg);
    return NULL;
}

int wasi_run(int (*fn)(void*), void* arg, uintptr_t* stack_low) {
    // a large stack, so deep guest recursion fails by a trap, not a crash
    size_t size = (size_t)1 << 30;
    void* stack = aligned_alloc(4096, size);

    if (!stack) {
        perror("stack");
        exit(1);
    }

    *stack_low = (uintptr_t)stack;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstack(&attr, stack, size);
    struct run r = {fn, arg, 0};
    pthread_t t;

    if (pthread_create(&t, &attr, trampoline, &r) != 0) {
        perror("pthread_create");
        exit(1);
    }

    pthread_join(t, NULL);
    return r.result;
}
