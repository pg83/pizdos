// devices: the devices every system has, a service of rootfs's /dev/init
// (service.h): null, which reads nothing; zero, which reads zeros; random
// and urandom, which read the kernel's entropy. Each takes everything
// written to it. An open of one is an endpoint of this process, rootfs
// hands it on: a READ's bytes go straight into the reader's memory,
// through the memfd it brings, and a WRITE's are never looked at.
//
//     devices [-r fd]
//
// -r: a descriptor to close once the devices are there; rootfs starts the
// shell when it has been.

#include <sys/param.h>
#include <sys/stat.h>

#include "service.h"

namespace {
    enum class Kind { Null, Zero, Random };

    struct Device {
        const char* name;
        Kind kind;
        service::Device registered;
    };

    Device devices[] = {
        {"null", Kind::Null, {}},
        {"zero", Kind::Zero, {}},
        {"random", Kind::Random, {}},
        {"urandom", Kind::Random, {}},
    };

    const unsigned char zeros[65536] = {};
    unsigned char entropy[65536];

    // As many bytes as asked go into the reader's memory: zeros, or the
    // kernel's random bytes; none of null.
    void readDevice(const Device& d, const service::Endpoint& e, const service::Request& r) {
        uint32_t woff;
        if (!e.reads) {
            return service::fail(r.reply(), EBADF);
        }
        if (r.window() < 0 || !r.at(8, woff)) {
            return service::fail(r.reply(), EINVAL);
        }
        const size_t n = d.kind == Kind::Null ? 0 : r.h.arg;
        for (size_t at = 0; at < n; at += sizeof zeros) {
            const size_t count = MIN(sizeof zeros, n - at);
            const unsigned char* bytes = zeros;
            if (d.kind == Kind::Random) {
                for (size_t k = 0; k < count; k += 256) {
                    if (getentropy(entropy + k, MIN(size_t(256), count - k)) != 0) {
                        return service::fail(r.reply(), EIO);
                    }
                }
                bytes = entropy;
            }
            if (pwrite(r.window(), bytes, count, off_t(woff) + off_t(at)) != ssize_t(count)) {
                return service::fail(r.reply(), EIO);
            }
        }
        const uint32_t count = uint32_t(n);
        service::answer(r.reply(), opOk, 0, &count, sizeof count);
    }

    // Everything written is taken, where it was written.
    void writeDevice(const service::Endpoint& e, const service::Request& r) {
        uint64_t off;
        uint32_t length;
        if (!e.writes) {
            return service::fail(r.reply(), EBADF);
        }
        if (!r.at(0, off) || !r.at(8, length)) {
            return service::fail(r.reply(), EINVAL);
        }
        unsigned char taken[12];
        memcpy(taken, &length, 4);
        memcpy(taken + 4, &off, 8);
        service::answer(r.reply(), opOk, 0, taken, sizeof taken);
    }

    bool handle(service::Endpoint& e, service::Request& r) {
        const Device& d = *static_cast<Device*>(e.of);
        const Stat& st = d.registered.st;
        if (e.channel) {
            service::answer(r.reply(), opOk, 0, &st, sizeof st);
            return true;
        }
        switch (r.h.op) {
        case opRead:
            readDevice(d, e, r);
            break;
        case opWrite:
            writeDevice(e, r);
            break;
        case opFstat:
            service::answer(r.reply(), opOk, 0, &st, sizeof st);
            break;
        case opFsync:
            service::answer(r.reply(), opOk, 0);
            break;
        case opFchmod:
        case opFchown:
        case opFutimes:
            service::fail(r.reply(), EPERM);
            break;
        default:
            service::fail(r.reply(), EINVAL);
        }
        return false;
    }
}

int main(int argc, char** argv) {
    int first;
    const int ready = service::ready(argc, argv, first);
    if (first != argc) {
        dprintf(STDERR_FILENO, "usage: devices [-r fd]\n");
        return 2;
    }
    const int epoll = epoll_create1(EPOLL_CLOEXEC);
    if (epoll < 0) {
        service::fatal("devices: epoll_create1: errno %d", errno);
    }
    for (Device& d : devices) {
        d.registered = service::enroll("devices", d.name, S_IFCHR | 0666);
        service::watch(epoll, {d.registered.end, true, false, false, &d});
    }
    if (ready >= 0) {
        close(ready);
    }
    service::serve(epoll, handle);
}
