#pragma once

#include <__base.h>

__BEGIN_DECLS

struct stat {
    dev_t st_dev;
    ino_t st_ino;
    mode_t st_mode;
    nlink_t st_nlink;
    uid_t st_uid;
    gid_t st_gid;
    dev_t st_rdev;
    off_t st_size;
    blksize_t st_blksize;
    int __pad0;
    blkcnt_t st_blocks;
    struct timespec st_atim;
    struct timespec st_mtim;
    struct timespec st_ctim;
};

#define st_atime st_atim.tv_sec
#define st_mtime st_mtim.tv_sec
#define st_ctime st_ctim.tv_sec

#define S_IFMT 0170000
#define S_IFDIR 0040000
#define S_IFCHR 0020000
#define S_IFBLK 0060000
#define S_IFREG 0100000
#define S_IFIFO 0010000
#define S_IFLNK 0120000
#define S_IFSOCK 0140000

#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m) (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m) (((m) & S_IFMT) == S_IFBLK)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISLNK(m) (((m) & S_IFMT) == S_IFLNK)
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)

#define S_ISUID 04000
#define S_ISGID 02000
#define S_ISVTX 01000
#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IXUSR 0100
#define S_IRWXU 0700
#define S_IRGRP 0040
#define S_IWGRP 0020
#define S_IXGRP 0010
#define S_IRWXG 0070
#define S_IROTH 0004
#define S_IWOTH 0002
#define S_IXOTH 0001
#define S_IRWXO 0007
#define S_IREAD S_IRUSR
#define S_IWRITE S_IWUSR
#define S_IEXEC S_IXUSR
#define ACCESSPERMS (S_IRWXU | S_IRWXG | S_IRWXO)
#define ALLPERMS (S_ISUID | S_ISGID | S_ISVTX | S_IRWXU | S_IRWXG | S_IRWXO)
#define DEFFILEMODE (S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH)

#define UTIME_NOW ((1l << 30) - 1l)
#define UTIME_OMIT ((1l << 30) - 2l)

int stat(const char* __restrict, struct stat* __restrict) __KERNEL(stat);
int fstat(int, struct stat*) __KERNEL(fstat);
int lstat(const char* __restrict, struct stat* __restrict) __KERNEL(lstat);
int fstatat(int, const char* __restrict, struct stat* __restrict, int) __KERNEL(fstatat);
int chmod(const char*, mode_t) __KERNEL(chmod);
int fchmod(int, mode_t) __KERNEL(fchmod);
int fchmodat(int, const char*, mode_t, int) __KERNEL(fchmodat);
int mkdir(const char*, mode_t) __KERNEL(mkdir);
int mkdirat(int, const char*, mode_t) __KERNEL(mkdirat);
int mkfifo(const char*, mode_t) __KERNEL(mkfifo);
int mkfifoat(int, const char*, mode_t) __KERNEL(mkfifoat);
int mknod(const char*, mode_t, dev_t) __KERNEL(mknod);
int mknodat(int, const char*, mode_t, dev_t) __KERNEL(mknodat);
mode_t umask(mode_t) __KERNEL(umask);
int utimensat(int, const char*, const struct timespec[2], int) __KERNEL(utimensat);
int futimens(int, const struct timespec[2]) __KERNEL(futimens);

__END_DECLS
