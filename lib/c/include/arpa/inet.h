#pragma once

#include <netinet/in.h>

__BEGIN_DECLS

in_addr_t inet_addr(const char*) __KERNEL(inet_addr);
in_addr_t inet_network(const char*) __KERNEL(inet_network);
int inet_aton(const char*, struct in_addr*) __KERNEL(inet_aton);
char* inet_ntoa(struct in_addr) __KERNEL(inet_ntoa);
int inet_pton(int, const char* __restrict, void* __restrict) __KERNEL(inet_pton);
const char* inet_ntop(int, const void* __restrict, char* __restrict, socklen_t) __KERNEL(inet_ntop);
struct in_addr inet_makeaddr(in_addr_t, in_addr_t) __KERNEL(inet_makeaddr);
in_addr_t inet_lnaof(struct in_addr) __KERNEL(inet_lnaof);
in_addr_t inet_netof(struct in_addr) __KERNEL(inet_netof);

__END_DECLS
