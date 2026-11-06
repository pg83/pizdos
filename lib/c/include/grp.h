#pragma once

#include <__base.h>

__BEGIN_DECLS

typedef struct _IO_FILE FILE;

struct group {
    char* gr_name;
    char* gr_passwd;
    gid_t gr_gid;
    char** gr_mem;
};

struct group* getgrnam(const char*) __KERNEL(getgrnam);
struct group* getgrgid(gid_t) __KERNEL(getgrgid);
int getgrnam_r(const char*, struct group*, char*, size_t, struct group**) __KERNEL(getgrnam_r);
int getgrgid_r(gid_t, struct group*, char*, size_t, struct group**) __KERNEL(getgrgid_r);
struct group* getgrent(void) __KERNEL(getgrent);
void setgrent(void) __KERNEL(setgrent);
void endgrent(void) __KERNEL(endgrent);
struct group* fgetgrent(FILE*);
int getgrouplist(const char*, gid_t, gid_t*, int*) __KERNEL(getgrouplist);
int initgroups(const char*, gid_t) __KERNEL(initgroups);
int setgroups(size_t, const gid_t*) __KERNEL(setgroups);

__END_DECLS
