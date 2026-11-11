//@ compile-flags: -g -D_GNU_SOURCE
//@ stdout: "address conversion: ok\n"

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>

int main() {
    in_addr address{};
    char text[128];
    const char* loopbacks[] = {"127.0.0.1", "127.1", "2130706433", "0x7f000001", "0177.0.0.1", "0X7F.0.0.1"};
    for (const char* loopback : loopbacks) {
        assert(inet_aton(loopback, &address) == 1 && ntohl(address.s_addr) == 0x7f000001);
        assert(inet_addr(loopback) == address.s_addr && strcmp(inet_ntoa(address), "127.0.0.1") == 0);
    }
    assert(inet_network("1.2.3.4") == 0x01020304 && inet_network("bad") == INADDR_NONE);
    const char* invalid[] = {"", "-1", "1.2.3.", "1.2.3.4.5", "256.1.2.3", "1.2.65536", "1.16777216", "08.1", "a.2"};
    for (const char* input : invalid) {
        assert(inet_aton(input, &address) == 0 && inet_addr(input) == INADDR_NONE);
    }
    assert(inet_pton(AF_INET, "192.0.2.1", &address) == 1 && ntohl(address.s_addr) == 0xc0000201);
    assert(inet_ntop(AF_INET, &address, text, sizeof(text)) == text && strcmp(text, "192.0.2.1") == 0);
    assert(inet_ntop(AF_INET, &address, text, 1) == nullptr && errno == ENOSPC);
    assert(inet_pton(AF_INET, "127.001.0.1", &address) == 0);
    assert(inet_pton(AF_INET, "300.0.0.1", &address) == 0);
    assert(inet_pton(999, "127.0.0.1", &address) == -1 && errno == EAFNOSUPPORT);
    assert(inet_ntop(999, &address, text, sizeof(text)) == nullptr && errno == EAFNOSUPPORT);
    in6_addr address6{};
    assert(inet_pton(AF_INET6, "2001:db8::1", &address6) == 1);
    assert(inet_ntop(AF_INET6, &address6, text, sizeof(text)) == text && strcmp(text, "2001:db8::1") == 0);
    assert(inet_pton(AF_INET6, "::ffff:192.0.2.1", &address6) == 1);
    assert(inet_ntop(AF_INET6, &address6, text, sizeof(text)) == text && strcmp(text, "::ffff:192.0.2.1") == 0);
    assert(inet_pton(AF_INET6, "fe80::1%lo", &address6) == 0);
    assert(inet_pton(AF_INET6, "not-an-address", &address6) == 0);
    struct if_nameindex* interfaces = if_nameindex();
    assert(interfaces && interfaces[0].if_index != 0);
    for (const struct if_nameindex* entry = interfaces; entry->if_index; ++entry) {
        assert(entry->if_name && if_nametoindex(entry->if_name) == entry->if_index);
        assert(if_indextoname(entry->if_index, text) == text && strcmp(text, entry->if_name) == 0);
    }
    if_freenameindex(interfaces);
    assert(if_nametoindex("pizdos-noif") == 0);
    assert(if_indextoname(0, text) == nullptr);

    assert(inet_aton("4294967296", &address) == 0);
    assert(inet_aton("0x100000000", &address) == 0);
    puts("address conversion: ok");
}
