#pragma once

#include <__base.h>

__BEGIN_DECLS

typedef struct {
    int __val[2];
} fsid_t;

struct statfs {
    unsigned long f_type;
    unsigned long f_bsize;
    fsblkcnt_t f_blocks;
    fsblkcnt_t f_bfree;
    fsblkcnt_t f_bavail;
    fsfilcnt_t f_files;
    fsfilcnt_t f_ffree;
    fsid_t f_fsid;
    unsigned long f_namelen;
    unsigned long f_frsize;
    unsigned long f_flags;
    unsigned long f_spare[4];
};

int statfs(const char*, struct statfs*) __KERNEL(statfs);
int fstatfs(int, struct statfs*) __KERNEL(fstatfs);

__END_DECLS
