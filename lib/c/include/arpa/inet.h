#pragma once

#include <netinet/in.h>

__BEGIN_DECLS

in_addr_t inet_addr(const char*);
in_addr_t inet_network(const char*);
int inet_aton(const char*, struct in_addr*);
char* inet_ntoa(struct in_addr);
int inet_pton(int, const char* __restrict, void* __restrict);
const char* inet_ntop(int, const void* __restrict, char* __restrict, socklen_t);
struct in_addr inet_makeaddr(in_addr_t, in_addr_t);
in_addr_t inet_lnaof(struct in_addr);
in_addr_t inet_netof(struct in_addr);

__END_DECLS
