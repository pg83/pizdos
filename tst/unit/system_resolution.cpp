//@ compile-flags: -g -D_GNU_SOURCE -fno-builtin
//@ stdout: "system resolution: ok\n"

#include <arpa/inet.h>
#include <assert.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>

int main() {
    // With no guest hosts file, localhost falls back to the host's resolver.
    // The standard loopback name and an invalid long label need no DNS server.
    addrinfo hints{}, *list = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_CANONNAME | AI_NUMERICSERV;
    assert(getaddrinfo("localhost", "12345", &hints, &list) == 0 && list);
    assert(list->ai_canonname && list->ai_canonname[0]);
    bool found = false;
    for (addrinfo* item = list; item; item = item->ai_next) {
        assert(item->ai_family == AF_INET && item->ai_socktype == SOCK_STREAM && item->ai_protocol == IPPROTO_TCP);
        assert(item->ai_addrlen >= sizeof(sockaddr_in));
        const auto* address = reinterpret_cast<const sockaddr_in*>(item->ai_addr);
        assert(ntohs(address->sin_port) == 12345);
        found = found || ntohl(address->sin_addr.s_addr) == 0x7f000001;
    }
    assert(found);
    freeaddrinfo(list);

    hostent* host = gethostbyname("localhost");
    assert(host && host->h_name && host->h_name[0] && host->h_addrtype == AF_INET && host->h_length == 4);
    in_addr loopback{};
    assert(inet_pton(AF_INET, "127.0.0.1", &loopback) == 1);
    found = false;
    for (char** address = host->h_addr_list; *address; ++address) {
        found = found || memcmp(*address, &loopback, 4) == 0;
    }
    assert(found);
    host = gethostbyaddr(&loopback, sizeof(loopback), AF_INET);
    assert(host && host->h_name && host->h_name[0] && host->h_addrtype == AF_INET);

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(12345);
    address.sin_addr = loopback;
    char name[NI_MAXHOST], service[NI_MAXSERV];
    assert(getnameinfo(reinterpret_cast<sockaddr*>(&address), sizeof(address), name, sizeof(name), service, sizeof(service), NI_NAMEREQD | NI_NUMERICSERV) == 0);
    assert(name[0] && strcmp(name, "127.0.0.1") != 0 && strcmp(service, "12345") == 0);

    in6_addr loopback6{};
    assert(inet_pton(AF_INET6, "::1", &loopback6) == 1);
    host = gethostbyname2("localhost", AF_INET6);
    assert(host && host->h_addrtype == AF_INET6 && host->h_length == 16);
    found = false;
    for (char** at = host->h_addr_list; *at; ++at) {
        found = found || memcmp(*at, &loopback6, sizeof(loopback6)) == 0;
    }
    assert(found);

    char invalid[301];
    memset(invalid, 'a', sizeof(invalid) - 1);
    invalid[sizeof(invalid) - 1] = 0;
    assert(getaddrinfo(invalid, "0", &hints, &list) == EAI_NONAME);
    assert(gethostbyname(invalid) == nullptr && h_errno == HOST_NOT_FOUND);
    puts("system resolution: ok");
}
