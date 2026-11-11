//@ host: false
//@ compile-flags: -g
//@ stdout: "classful addresses: ok\n"

#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

// BSD's legacy address API uses network byte order for struct in_addr
// and host byte order for the separate network and host numbers.
// The host musl 1.2.5 omits those byte-order conversions, so it cannot
// serve as a reference for these three functions.
int main() {
    const char* classes[] = {"10.1.2.3", "172.16.18.52", "192.168.1.2"};
    const in_addr_t networks[] = {10, 0xac10, 0xc0a801};
    const in_addr_t hosts[] = {0x010203, 0x1234, 2};
    in_addr address{};
    for (unsigned i = 0; i < 3; ++i) {
        assert(inet_aton(classes[i], &address) == 1);
        assert(inet_netof(address) == networks[i] && inet_lnaof(address) == hosts[i]);
        assert(inet_makeaddr(networks[i], hosts[i]).s_addr == address.s_addr);
    }
    assert(strcmp(inet_ntoa(inet_makeaddr(10, 0xff010203)), "10.1.2.3") == 0);
    assert(strcmp(inet_ntoa(inet_makeaddr(0xac10, 0xffff1234)), "172.16.18.52") == 0);
    assert(strcmp(inet_ntoa(inet_makeaddr(0xc0a801, 0xffffff02)), "192.168.1.2") == 0);
    assert(strcmp(inet_ntoa(inet_makeaddr(0xe0000000, 1)), "224.0.0.1") == 0);
    puts("classful addresses: ok");
}
