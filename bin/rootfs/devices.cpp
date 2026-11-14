// devices: the devices every system has, a service of rootfs's /dev/init
// (protocol.h): null, which reads nothing; zero, which reads zeros; random
// and urandom, which read the kernel's entropy. Each takes everything
// written to it. An open of one is an endpoint of this process, rootfs
// hands it on: a READ's bytes go straight into the reader's memory,
// through the memfd it brings, and a WRITE's are never looked at.
//
//     devices [-r fd]
//
// -r: a descriptor to close once the devices are there; rootfs starts the
// shell when it has been.

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "protocol.h"

namespace {
    enum class Kind { Null, Zero, Random };

    struct Device {
        const char* name;
        Kind kind;
        int end;  // ours of the pair rootfs hands the device's opens on to
        Stat st;  // the device's, as rootfs has its name
    };

    Device devices[] = {
        {"null", Kind::Null, -1, {}},
        {"zero", Kind::Zero, -1, {}},
        {"random", Kind::Random, -1, {}},
        {"urandom", Kind::Random, -1, {}},
    };

    // What an event is of: a device's end, where its opens come, or an
    // open file of it, what its open lets it do.
    struct Watch {
        int fd;
        Device* device;
        bool open;
        bool reads;
        bool writes;
    };

    alignas(Header) unsigned char packet[65536];
    const unsigned char zeros[65536] = {};
    unsigned char entropy[65536];

    [[noreturn, gnu::format(printf, 1, 2)]] void fatal(const char* format, ...) {
        va_list args;
        va_start(args, format);
        vdprintf(STDERR_FILENO, format, args);
        va_end(args);
        dprintf(STDERR_FILENO, "\n");
        exit(1);
    }

    void watch(int epoll, Watch w) {
        auto* at = static_cast<Watch*>(malloc(sizeof(Watch)));
        if (!at) {
            fatal("devices: out of memory");
        }
        *at = w;
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.ptr = at;
        if (epoll_ctl(epoll, EPOLL_CTL_ADD, w.fd, &ev) != 0) {
            fatal("devices: epoll_ctl: errno %d", errno);
        }
    }

    void answer(int fd, uint32_t op, uint32_t arg, const void* data = nullptr, size_t size = 0) {
        Header h{op, arg, 0, 0};
        const iovec parts[] = {{&h, sizeof h}, {const_cast<void*>(data), size}};
        sendPacket(fd, parts, size ? 2 : 1, nullptr, 0);  // a reader gone is its own loss
    }

    void fail(int fd, int e) {
        answer(fd, opErr, uint32_t(e));
    }

    // Registers a device on reg, an endpoint of /dev/init: the far end of
    // a pair goes with the request, the device's stat comes back.
    void enroll(int reg, Device& d) {
        int pair[2];
        int reply[2];
        if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair) != 0 ||
            socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, reply) != 0) {
            fatal("devices: socketpair: errno %d", errno);
        }
        Header h{opRegister, S_IFCHR | 0666, geteuid(), getegid()};
        const iovec parts[] = {{&h, sizeof h}, {const_cast<char*>(d.name), strlen(d.name)}};
        const int fds[] = {pair[1], reply[1]};
        if (sendPacket(reg, parts, 2, fds, 2) < 0) {
            fatal("devices: /dev/init: errno %d", errno);
        }
        close(pair[1]);
        close(reply[1]);
        struct {
            Header h;
            Stat st;
        } got;
        const ssize_t n = recv(reply[0], &got, sizeof got, 0);
        close(reply[0]);
        if (n == ssize_t(sizeof got) && got.h.op == opOk) {
            d.end = pair[0];
            d.st = got.st;
            return;
        }
        fatal("devices: /dev/%s: errno %d", d.name, n >= ssize_t(sizeof(Header)) && got.h.op == opErr ? int(got.h.arg) : EIO);
    }

    // As many bytes as asked go into the reader's memory: zeros, or the
    // kernel's random bytes; none of null.
    void readDevice(const Watch& w, const Header& h, int window, int fd) {
        if (!w.reads) {
            return fail(fd, EBADF);
        }
        if (window < 0) {
            return fail(fd, EINVAL);
        }
        const size_t n = w.device->kind == Kind::Null ? 0 : h.arg;
        for (size_t at = 0; at < n; at += sizeof zeros) {
            const size_t count = MIN(sizeof zeros, n - at);
            const unsigned char* bytes = zeros;
            if (w.device->kind == Kind::Random) {
                for (size_t k = 0; k < count; k += 256) {
                    if (getentropy(entropy + k, MIN(size_t(256), count - k)) != 0) {
                        return fail(fd, EIO);
                    }
                }
                bytes = entropy;
            }
            if (pwrite(window, bytes, count, off_t(at)) != ssize_t(count)) {
                return fail(fd, EIO);
            }
        }
        const uint32_t count = uint32_t(n);
        answer(fd, opOk, 0, &count, sizeof count);
    }

    // Everything written is taken, where it was written.
    void writeDevice(const Watch& w, const unsigned char* body, size_t size, int fd) {
        if (!w.writes) {
            return fail(fd, EBADF);
        }
        if (size < 12) {
            return fail(fd, EINVAL);
        }
        unsigned char taken[12];
        memcpy(taken, body + 8, 4);
        memcpy(taken + 4, body, 8);
        answer(fd, opOk, 0, taken, sizeof taken);
    }

    // A packet on a watched endpoint. On a device's end it is an OPEN rootfs
    // handed on, whose endpoint is the open file's from then on.
    void serve(int epoll, Watch* w) {
        int fds[maxFds];
        size_t count = 0;
        const ssize_t n = receivePacket(w->fd, packet, sizeof packet, fds, count);
        if (n <= 0) {
            if (!w->open) {
                exit(0);  // rootfs let go of the device: the system ends
            }
            epoll_ctl(epoll, EPOLL_CTL_DEL, w->fd, nullptr);
            close(w->fd);
            free(w);
            return;
        }
        const int fd = count > 0 ? fds[count - 1] : -1;
        Header h{};
        if (fd < 0 || size_t(n) < sizeof h) {
            for (size_t i = 0; i < count; ++i) {
                close(fds[i]);
            }
            return;
        }
        memcpy(&h, packet, sizeof h);
        const unsigned char* body = packet + sizeof h;
        const size_t size = size_t(n) - sizeof h;
        const int window = count >= 2 ? fds[0] : -1;
        bool kept = false;
        if (!w->open) {
            if (h.op == opOpen) {
                const uint32_t access = h.arg & O_ACCMODE;
                watch(epoll, {fd, w->device, true, access != O_WRONLY, access != O_RDONLY});
                answer(fd, opOk, 0, &w->device->st, sizeof(Stat));
                kept = true;
            } else {
                fail(fd, EINVAL);
            }
        } else {
            switch (h.op) {
            case opRead:
                readDevice(*w, h, window, fd);
                break;
            case opWrite:
                writeDevice(*w, body, size, fd);
                break;
            case opFstat:
                answer(fd, opOk, 0, &w->device->st, sizeof(Stat));
                break;
            case opFsync:
                answer(fd, opOk, 0);
                break;
            case opFchmod:
            case opFchown:
            case opFutimes:
                fail(fd, EPERM);
                break;
            default:
                fail(fd, EINVAL);
            }
        }
        for (size_t i = 0; i < count; ++i) {
            if (!kept || fds[i] != fd) {
                close(fds[i]);
            }
        }
    }
}

int main(int argc, char** argv) {
    int ready = -1;
    if (argc == 3 && strcmp(argv[1], "-r") == 0) {
        ready = atoi(argv[2]);
    } else if (argc != 1) {
        dprintf(STDERR_FILENO, "usage: devices [-r fd]\n");
        return 2;
    }
    const int reg = open("/dev/init", O_RDWR | O_CLOEXEC);
    if (reg < 0) {
        fatal("devices: /dev/init: errno %d", errno);
    }
    const int epoll = epoll_create1(EPOLL_CLOEXEC);
    if (epoll < 0) {
        fatal("devices: epoll_create1: errno %d", errno);
    }
    for (Device& d : devices) {
        enroll(reg, d);
        watch(epoll, {d.end, &d, false, false, false});
    }
    close(reg);
    if (ready >= 0) {
        close(ready);
    }
    for (;;) {
        epoll_event events[16];
        const int n = epoll_wait(epoll, events, 16, -1);
        if (n < 0 && errno != EINTR) {
            fatal("devices: epoll_wait: errno %d", errno);
        }
        for (int i = 0; i < n; ++i) {
            serve(epoll, static_cast<Watch*>(events[i].data.ptr));
        }
    }
}
