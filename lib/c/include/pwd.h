#pragma once

#include <__base.h>

__BEGIN_DECLS

typedef struct _IO_FILE FILE;

struct passwd {
    char* pw_name;
    char* pw_passwd;
    uid_t pw_uid;
    gid_t pw_gid;
    char* pw_gecos;
    char* pw_dir;
    char* pw_shell;
};

struct passwd* getpwnam(const char*);
struct passwd* getpwuid(uid_t);
int getpwnam_r(const char*, struct passwd*, char*, size_t, struct passwd**);
int getpwuid_r(uid_t, struct passwd*, char*, size_t, struct passwd**);
struct passwd* getpwent(void);
void setpwent(void);
void endpwent(void);
struct passwd* fgetpwent(FILE*);
int putpwent(const struct passwd*, FILE*);

__END_DECLS
