// A service of one device for tst/test_devices.py: it registers name at
// rootfs's /dev/init with the mode given in octal, says so, and serves the
// device in a child whose pid is /tmp/<name>.pid: the device's bytes are
// the text given, read at any offset into the reader's memory, through the
// memfd a READ brings. ro registers on an endpoint of /dev/init open to
// read only.
//
// copy reads n bytes of a device at off into a memfd of 16 bytes of its
// memory, at woff, with copy_file_range, and prints the count, the offsets
// after it and the bytes; put writes a text from a memfd of its memory to
// a device with copy_file_range and prints the count.
//
//     devices_service <name> <mode> <text> [ro]
//     devices_service copy <device> <off> <woff> <n>
//     devices_service put <device> <text>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../bin/rootfs/protocol.h"

namespace {
    alignas(Header) unsigned char packet[65536];

    struct Open {
        int fd;
        bool channel;
        bool writeOnly;
    };

    void answer(int fd, uint32_t op, uint32_t arg, const void* data = nullptr, size_t size = 0) {
        Header h{op, arg, 0, 0};
        const iovec parts[] = {{&h, sizeof h}, {const_cast<void*>(data), size}};
        sendPacket(fd, parts, size ? 2 : 1, nullptr, 0);
    }

    void watch(int epoll, Open o) {
        auto* at = static_cast<Open*>(malloc(sizeof(Open)));
        *at = o;
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.ptr = at;
        epoll_ctl(epoll, EPOLL_CTL_ADD, o.fd, &ev);
    }
}

int main(int argc, char** argv) {
    if (argc == 6 && strcmp(argv[1], "copy") == 0) {
        char bytes[16];
        memset(bytes, '.', sizeof bytes);
        const int device = open(argv[2], O_RDONLY);
        const int window = memfd_create(bytes, sizeof bytes, O_RDWR);
        off_t from = atoll(argv[3]);
        off_t to = atoll(argv[4]);
        const ssize_t n = copy_file_range(device, &from, window, &to, size_t(atoll(argv[5])), 0);
        printf("%zd %lld %lld %.16s\n", n, (long long)from, (long long)to, bytes);
        return 0;
    }
    if (argc == 4 && strcmp(argv[1], "put") == 0) {
        const int device = open(argv[2], O_WRONLY);
        const int window = memfd_create(argv[3], strlen(argv[3]), O_RDONLY);
        printf("%zd\n", copy_file_range(window, nullptr, device, nullptr, strlen(argv[3]), 0));
        return 0;
    }
    if (argc < 4) {
        dprintf(STDERR_FILENO, "usage: devices_service <name> <mode> <text> [ro]\n");
        return 2;
    }
    const char* name = argv[1];
    const uint32_t mode = uint32_t(strtoul(argv[2], nullptr, 8));
    const char* text = argv[3];
    const size_t length = strlen(text);
    const int reg = open("/dev/init", argc > 4 ? O_RDONLY : O_RDWR);
    int pair[2];
    int reply[2];
    if (reg < 0 || socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair) != 0 || socketpair(AF_UNIX, SOCK_SEQPACKET, 0, reply) != 0) {
        perror("devices_service");
        return 1;
    }
    Header h{opRegister, mode, geteuid(), getegid()};
    const iovec parts[] = {{&h, sizeof h}, {const_cast<char*>(name), strlen(name)}};
    const int fds[] = {pair[1], reply[1]};
    sendPacket(reg, parts, 2, fds, 2);
    close(pair[1]);
    close(reply[1]);
    close(reg);
    struct {
        Header h;
        Stat st;
    } registered;
    const ssize_t n = recv(reply[0], &registered, sizeof registered, 0);
    if (n != ssize_t(sizeof registered) || registered.h.op != opOk) {
        printf("register %s: %s\n", name, strerror(n >= ssize_t(sizeof(Header)) ? int(registered.h.arg) : EIO));
        return 1;
    }
    printf("registered %s\n", name);
    fflush(stdout);
    const pid_t child = fork();
    if (child != 0) {
        char path[64];
        snprintf(path, sizeof path, "/tmp/%s.pid", name);
        FILE* f = fopen(path, "w");
        fprintf(f, "%d", int(child));
        fclose(f);
        return 0;
    }
    const int epoll = epoll_create1(0);
    watch(epoll, {pair[0], true, false});
    for (;;) {
        epoll_event ev;
        if (epoll_wait(epoll, &ev, 1, -1) != 1) {
            continue;
        }
        Open* o = static_cast<Open*>(ev.data.ptr);
        int in[maxFds];
        size_t count = 0;
        const ssize_t got = receivePacket(o->fd, packet, sizeof packet, in, count);
        if (got <= 0) {
            if (o->channel) {
                return 0;
            }
            epoll_ctl(epoll, EPOLL_CTL_DEL, o->fd, nullptr);
            close(o->fd);
            free(o);
            continue;
        }
        if (count == 0 || size_t(got) < sizeof(Header)) {
            continue;
        }
        Header q;
        memcpy(&q, packet, sizeof q);
        const int fd = in[count - 1];
        if (o->channel) {
            // An OPEN rootfs handed on: its endpoint is the open file's.
            watch(epoll, {fd, false, (q.arg & O_ACCMODE) == O_WRONLY});
            answer(fd, opOk, 0, &registered.st, sizeof registered.st);
            continue;
        }
        if (q.op == opRead && o->writeOnly) {
            answer(fd, opErr, EBADF);
        } else if (q.op == opRead && count == 2) {
            uint64_t off;
            uint32_t woff;
            memcpy(&off, packet + sizeof q, sizeof off);
            memcpy(&woff, packet + sizeof q + sizeof off, sizeof woff);
            const size_t part = off < length ? MIN(size_t(q.arg), length - size_t(off)) : 0;
            pwrite(in[0], text + MIN(off, uint64_t(length)), part, woff);
            const uint32_t done = uint32_t(part);
            answer(fd, opOk, 0, &done, sizeof done);
        } else if (q.op == opFstat) {
            answer(fd, opOk, 0, &registered.st, sizeof registered.st);
        } else {
            answer(fd, opErr, EINVAL);
        }
        for (size_t i = 0; i < count; ++i) {
            close(in[i]);
        }
    }
}
