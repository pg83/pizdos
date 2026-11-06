#include "stdio_impl.h"

#include "libc.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * The parts of musl's stdio that talk to the system, rewritten over kernel
 * imports. The process is single-threaded, so FILE locks are never taken:
 * every FILE has lock == -1 and the locking entry points do nothing.
 */

size_t __stdio_read(FILE* f, unsigned char* buf, size_t len) {
    struct iovec iov[2] = {
        {.iov_base = buf, .iov_len = len - !!f->buf_size},
        {.iov_base = f->buf, .iov_len = f->buf_size},
    };
    ssize_t cnt;

    if (iov[0].iov_len) {
        cnt = readv(f->fd, iov, 2);
    } else {
        cnt = read(f->fd, iov[1].iov_base, iov[1].iov_len);
    }

    if (cnt <= 0) {
        f->flags |= cnt ? F_ERR : F_EOF;

        return 0;
    }

    if ((size_t)cnt <= iov[0].iov_len) {
        return cnt;
    }

    cnt -= iov[0].iov_len;
    f->rpos = f->buf;
    f->rend = f->buf + cnt;

    if (f->buf_size) {
        buf[len - 1] = *f->rpos++;
    }

    return len;
}

size_t __stdio_write(FILE* f, const unsigned char* buf, size_t len) {
    struct iovec iovs[2] = {
        {.iov_base = f->wbase, .iov_len = (size_t)(f->wpos - f->wbase)},
        {.iov_base = (void*)buf, .iov_len = len},
    };
    struct iovec* iov = iovs;
    size_t rem = iov[0].iov_len + iov[1].iov_len;
    int iovcnt = 2;

    for (;;) {
        ssize_t cnt = writev(f->fd, iov, iovcnt);

        if (cnt >= 0 && (size_t)cnt == rem) {
            f->wend = f->buf + f->buf_size;
            f->wpos = f->wbase = f->buf;

            return len;
        }

        if (cnt < 0) {
            f->wpos = f->wbase = f->wend = 0;
            f->flags |= F_ERR;

            return iovcnt == 2 ? 0 : len - iov[0].iov_len;
        }

        rem -= cnt;

        if ((size_t)cnt > iov[0].iov_len) {
            cnt -= iov[0].iov_len;
            iov++;
            iovcnt--;
        }

        iov[0].iov_base = (char*)iov[0].iov_base + cnt;
        iov[0].iov_len -= cnt;
    }
}

size_t __stdout_write(FILE* f, const unsigned char* buf, size_t len) {
    struct winsize wsz;

    f->write = __stdio_write;

    if (!(f->flags & F_SVB) && ioctl(f->fd, TIOCGWINSZ, &wsz)) {
        f->lbf = -1;
    }

    return __stdio_write(f, buf, len);
}

off_t __stdio_seek(FILE* f, off_t off, int whence) {
    return lseek(f->fd, off, whence);
}

int __stdio_close(FILE* f) {
    return close(f->fd);
}

FILE* __fdopen(int fd, const char* mode) {
    FILE* f;
    struct winsize wsz;

    if (!strchr("rwa", *mode)) {
        errno = EINVAL;

        return 0;
    }

    f = malloc(sizeof *f + UNGET + BUFSIZ);

    if (!f) {
        return 0;
    }

    memset(f, 0, sizeof *f);

    if (!strchr(mode, '+')) {
        f->flags = (*mode == 'r') ? F_NOWR : F_NORD;
    }

    if (strchr(mode, 'e')) {
        fcntl(fd, F_SETFD, FD_CLOEXEC);
    }

    if (*mode == 'a') {
        int flags = fcntl(fd, F_GETFL);

        if (!(flags & O_APPEND)) {
            fcntl(fd, F_SETFL, flags | O_APPEND);
        }

        f->flags |= F_APP;
    }

    f->fd = fd;
    f->buf = (unsigned char*)f + sizeof *f + UNGET;
    f->buf_size = BUFSIZ;
    f->lbf = EOF;

    if (!(f->flags & F_NOWR) && !ioctl(fd, TIOCGWINSZ, &wsz)) {
        f->lbf = '\n';
    }

    f->read = __stdio_read;
    f->write = __stdio_write;
    f->seek = __stdio_seek;
    f->close = __stdio_close;
    f->lock = -1;

    return __ofl_add(f);
}

weak_alias(__fdopen, fdopen);

FILE* fopen(const char* __restrict filename, const char* __restrict mode) {
    FILE* f;
    int fd;
    int flags;

    if (!strchr("rwa", *mode)) {
        errno = EINVAL;

        return 0;
    }

    flags = __fmodeflags(mode);
    fd = open(filename, flags, 0666);

    if (fd < 0) {
        return 0;
    }

    f = __fdopen(fd, mode);

    if (f) {
        return f;
    }

    close(fd);

    return 0;
}

FILE* freopen(const char* __restrict filename, const char* __restrict mode, FILE* __restrict f) {
    int flags = __fmodeflags(mode);
    FILE* other;

    fflush(f);

    if (!filename) {
        if (flags & O_CLOEXEC) {
            fcntl(f->fd, F_SETFD, FD_CLOEXEC);
        }

        flags &= ~(O_CREAT | O_EXCL | O_CLOEXEC);

        if (fcntl(f->fd, F_SETFL, flags) < 0) {
            fclose(f);

            return 0;
        }
    } else {
        other = fopen(filename, mode);

        if (!other) {
            fclose(f);

            return 0;
        }

        if (other->fd == f->fd) {
            other->fd = -1;
        } else if (dup3(other->fd, f->fd, flags & O_CLOEXEC) < 0) {
            fclose(other);
            fclose(f);

            return 0;
        }

        f->flags = (f->flags & F_PERM) | other->flags;
        f->read = other->read;
        f->write = other->write;
        f->seek = other->seek;
        f->close = other->close;
        fclose(other);
    }

    f->mode = 0;
    f->locale = 0;

    return f;
}

FILE* popen(const char* command, const char* mode) {
    int ends[2];
    int reading = *mode == 'r';
    FILE* f;
    pid_t pid;

    if (!reading && *mode != 'w') {
        errno = EINVAL;

        return 0;
    }

    if (pipe2(ends, O_CLOEXEC)) {
        return 0;
    }

    f = fdopen(ends[!reading], mode);

    if (!f) {
        close(ends[0]);
        close(ends[1]);

        return 0;
    }

    pid = vfork();

    if (pid == 0) {
        int child = ends[reading];

        if (child != reading) {
            dup2(child, reading);
        } else {
            fcntl(child, F_SETFD, 0);
        }

        execl("/bin/sh", "sh", "-c", command, (char*)0);
        _exit(127);
    }

    close(ends[reading]);

    if (pid < 0) {
        fclose(f);

        return 0;
    }

    f->pipe_pid = pid;

    return f;
}

int pclose(FILE* f) {
    int status;
    pid_t pid = f->pipe_pid;

    fclose(f);

    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return -1;
        }
    }

    return status;
}

int __lockfile(FILE* f) {
    (void)f;

    return 0;
}

void __unlockfile(FILE* f) {
    (void)f;
}

void flockfile(FILE* f) {
    (void)f;
}

int ftrylockfile(FILE* f) {
    (void)f;

    return 0;
}

void funlockfile(FILE* f) {
    (void)f;
}

void __unlist_locked_file(FILE* f) {
    (void)f;
}

struct __libc __libc;
