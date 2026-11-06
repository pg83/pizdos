#pragma once

#include <netinet/in.h>

__BEGIN_DECLS

struct hostent {
    char* h_name;
    char** h_aliases;
    int h_addrtype;
    int h_length;
    char** h_addr_list;
};

#define h_addr h_addr_list[0]

struct servent {
    char* s_name;
    char** s_aliases;
    int s_port;
    char* s_proto;
};

struct protoent {
    char* p_name;
    char** p_aliases;
    int p_proto;
};

struct netent {
    char* n_name;
    char** n_aliases;
    int n_addrtype;
    uint32_t n_net;
};

struct addrinfo {
    int ai_flags;
    int ai_family;
    int ai_socktype;
    int ai_protocol;
    socklen_t ai_addrlen;
    struct sockaddr* ai_addr;
    char* ai_canonname;
    struct addrinfo* ai_next;
};

#define AI_PASSIVE 0x01
#define AI_CANONNAME 0x02
#define AI_NUMERICHOST 0x04
#define AI_V4MAPPED 0x08
#define AI_ALL 0x10
#define AI_ADDRCONFIG 0x20
#define AI_NUMERICSERV 0x400

#define NI_NUMERICHOST 0x01
#define NI_NUMERICSERV 0x02
#define NI_NOFQDN 0x04
#define NI_NAMEREQD 0x08
#define NI_DGRAM 0x10
#define NI_NUMERICSCOPE 0x100
#define NI_MAXHOST 255
#define NI_MAXSERV 32

#define EAI_BADFLAGS (-1)
#define EAI_NONAME (-2)
#define EAI_AGAIN (-3)
#define EAI_FAIL (-4)
#define EAI_NODATA (-5)
#define EAI_FAMILY (-6)
#define EAI_SOCKTYPE (-7)
#define EAI_SERVICE (-8)
#define EAI_ADDRFAMILY (-9)
#define EAI_MEMORY (-10)
#define EAI_SYSTEM (-11)
#define EAI_OVERFLOW (-12)

#define HOST_NOT_FOUND 1
#define TRY_AGAIN 2
#define NO_RECOVERY 3
#define NO_DATA 4
#define NO_ADDRESS NO_DATA

extern int h_errno;

int getaddrinfo(const char* __restrict, const char* __restrict, const struct addrinfo* __restrict, struct addrinfo** __restrict) __KERNEL(getaddrinfo);
void freeaddrinfo(struct addrinfo*) __KERNEL(freeaddrinfo);
const char* gai_strerror(int) __KERNEL(gai_strerror);
int getnameinfo(const struct sockaddr* __restrict, socklen_t, char* __restrict, socklen_t, char* __restrict, socklen_t, int) __KERNEL(getnameinfo);
struct hostent* gethostbyname(const char*) __KERNEL(gethostbyname);
struct hostent* gethostbyname2(const char*, int) __KERNEL(gethostbyname2);
struct hostent* gethostbyaddr(const void*, socklen_t, int) __KERNEL(gethostbyaddr);
void sethostent(int) __KERNEL(sethostent);
void endhostent(void) __KERNEL(endhostent);
struct hostent* gethostent(void) __KERNEL(gethostent);
struct servent* getservbyname(const char*, const char*) __KERNEL(getservbyname);
struct servent* getservbyport(int, const char*) __KERNEL(getservbyport);
struct protoent* getprotobyname(const char*) __KERNEL(getprotobyname);
struct protoent* getprotobynumber(int) __KERNEL(getprotobynumber);
struct netent* getnetbyname(const char*) __KERNEL(getnetbyname);
struct netent* getnetbyaddr(uint32_t, int) __KERNEL(getnetbyaddr);
void herror(const char*) __KERNEL(herror);
const char* hstrerror(int) __KERNEL(hstrerror);

__END_DECLS
