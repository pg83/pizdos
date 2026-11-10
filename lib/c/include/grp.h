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

struct group* getgrnam(const char*);
struct group* getgrgid(gid_t);
int getgrnam_r(const char*, struct group*, char*, size_t, struct group**);
int getgrgid_r(gid_t, struct group*, char*, size_t, struct group**);
struct group* getgrent(void);
void setgrent(void);
void endgrent(void);
struct group* fgetgrent(FILE*);
int getgrouplist(const char*, gid_t, gid_t*, int*);
int initgroups(const char*, gid_t);
int setgroups(size_t, const gid_t*);

__END_DECLS
