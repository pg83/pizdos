#pragma once

#include <__base.h>

__BEGIN_DECLS

#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4

#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif
#define SEEK_DATA 3
#define SEEK_HOLE 4

#define F_ULOCK 0
#define F_LOCK 1
#define F_TLOCK 2
#define F_TEST 3

#define _POSIX_VERSION 200809L
#define _POSIX2_VERSION _POSIX_VERSION
#define _XOPEN_VERSION 700
#define _POSIX_VDISABLE 0
#define _POSIX_JOB_CONTROL 1
#define _POSIX_SAVED_IDS 1
#define _POSIX_MONOTONIC_CLOCK _POSIX_VERSION
#define _POSIX_TIMERS _POSIX_VERSION

#define _PC_LINK_MAX 0
#define _PC_MAX_CANON 1
#define _PC_MAX_INPUT 2
#define _PC_NAME_MAX 3
#define _PC_PATH_MAX 4
#define _PC_PIPE_BUF 5
#define _PC_CHOWN_RESTRICTED 6
#define _PC_NO_TRUNC 7
#define _PC_VDISABLE 8

#define _SC_ARG_MAX 0
#define _SC_CHILD_MAX 1
#define _SC_CLK_TCK 2
#define _SC_NGROUPS_MAX 3
#define _SC_OPEN_MAX 4
#define _SC_STREAM_MAX 5
#define _SC_TZNAME_MAX 6
#define _SC_JOB_CONTROL 7
#define _SC_SAVED_IDS 8
#define _SC_VERSION 29
#define _SC_PAGESIZE 30
#define _SC_PAGE_SIZE 30
#define _SC_LINE_MAX 43
#define _SC_IOV_MAX 60
#define _SC_GETGR_R_SIZE_MAX 69
#define _SC_GETPW_R_SIZE_MAX 70
#define _SC_LOGIN_NAME_MAX 71
#define _SC_TTY_NAME_MAX 72
#define _SC_NPROCESSORS_CONF 83
#define _SC_NPROCESSORS_ONLN 84
#define _SC_PHYS_PAGES 85
#define _SC_AVPHYS_PAGES 86
#define _SC_MONOTONIC_CLOCK 149
#define _SC_SYMLOOP_MAX 173
#define _SC_HOST_NAME_MAX 180

#define _CS_PATH 0

extern char** environ;
extern char* optarg;
extern int optind;
extern int opterr;
extern int optopt;
extern int optreset;

int getopt(int, char* const[], const char*);

ssize_t read(int, void*, size_t) __KERNEL(read);
ssize_t write(int, const void*, size_t) __KERNEL(write);
ssize_t pread(int, void*, size_t, off_t) __KERNEL(pread);
ssize_t pwrite(int, const void*, size_t, off_t) __KERNEL(pwrite);
off_t lseek(int, off_t, int) __KERNEL(lseek);
int close(int) __KERNEL(close);
int dup(int) __KERNEL(dup);
int dup2(int, int) __KERNEL(dup2);
int dup3(int, int, int) __KERNEL(dup3);
int pipe(int[2]) __KERNEL(pipe);
int pipe2(int[2], int) __KERNEL(pipe2);
int fsync(int) __KERNEL(fsync);
int fdatasync(int) __KERNEL(fdatasync);
void sync(void) __KERNEL(sync);
int syncfs(int) __KERNEL(syncfs);
int ftruncate(int, off_t) __KERNEL(ftruncate);
int truncate(const char*, off_t) __KERNEL(truncate);
int lockf(int, int, off_t) __KERNEL(lockf);

pid_t fork(void) __KERNEL(fork);
pid_t vfork(void) __RETURNS_TWICE __KERNEL(vfork);
int execve(const char*, char* const[], char* const[]) __KERNEL(execve);
int execv(const char*, char* const[]) __KERNEL(execv);
int execvp(const char*, char* const[]) __KERNEL(execvp);
int execvpe(const char*, char* const[], char* const[]) __KERNEL(execvpe);
int fexecve(int, char* const[], char* const[]) __KERNEL(fexecve);
int execl(const char*, const char*, ...) __KERNEL(execl);
int execlp(const char*, const char*, ...) __KERNEL(execlp);
int execle(const char*, const char*, ...) __KERNEL(execle);
__NORETURN void _exit(int) __KERNEL(_exit);

pid_t getpid(void) __KERNEL(getpid);
pid_t getppid(void) __KERNEL(getppid);
pid_t getpgrp(void) __KERNEL(getpgrp);
pid_t getpgid(pid_t) __KERNEL(getpgid);
int setpgid(pid_t, pid_t) __KERNEL(setpgid);
int setpgrp(void) __KERNEL(setpgrp);
pid_t getsid(pid_t) __KERNEL(getsid);
pid_t setsid(void) __KERNEL(setsid);
pid_t tcgetpgrp(int) __KERNEL(tcgetpgrp);
int tcsetpgrp(int, pid_t) __KERNEL(tcsetpgrp);

uid_t getuid(void) __KERNEL(getuid);
uid_t geteuid(void) __KERNEL(geteuid);
gid_t getgid(void) __KERNEL(getgid);
gid_t getegid(void) __KERNEL(getegid);
int setuid(uid_t) __KERNEL(setuid);
int seteuid(uid_t) __KERNEL(seteuid);
int setgid(gid_t) __KERNEL(setgid);
int setegid(gid_t) __KERNEL(setegid);
int setreuid(uid_t, uid_t) __KERNEL(setreuid);
int setregid(gid_t, gid_t) __KERNEL(setregid);
int getresuid(uid_t*, uid_t*, uid_t*) __KERNEL(getresuid);
int getresgid(gid_t*, gid_t*, gid_t*) __KERNEL(getresgid);
int setresuid(uid_t, uid_t, uid_t) __KERNEL(setresuid);
int setresgid(gid_t, gid_t, gid_t) __KERNEL(setresgid);
int getgroups(int, gid_t[]) __KERNEL(getgroups);

int access(const char*, int) __KERNEL(access);
int faccessat(int, const char*, int, int) __KERNEL(faccessat);
int euidaccess(const char*, int) __KERNEL(euidaccess);
int eaccess(const char*, int) __KERNEL(eaccess);
int chdir(const char*) __KERNEL(chdir);
int fchdir(int) __KERNEL(fchdir);
int chroot(const char*) __KERNEL(chroot);
char* getcwd(char*, size_t) __KERNEL(getcwd);
char* get_current_dir_name(void) __KERNEL(get_current_dir_name);
int unlink(const char*) __KERNEL(unlink);
int unlinkat(int, const char*, int) __KERNEL(unlinkat);
int rmdir(const char*) __KERNEL(rmdir);
int link(const char*, const char*) __KERNEL(link);
int linkat(int, const char*, int, const char*, int) __KERNEL(linkat);
int symlink(const char*, const char*) __KERNEL(symlink);
int symlinkat(const char*, int, const char*) __KERNEL(symlinkat);
ssize_t readlink(const char* __restrict, char* __restrict, size_t) __KERNEL(readlink);
ssize_t readlinkat(int, const char* __restrict, char* __restrict, size_t) __KERNEL(readlinkat);
int chown(const char*, uid_t, gid_t) __KERNEL(chown);
int fchown(int, uid_t, gid_t) __KERNEL(fchown);
int lchown(const char*, uid_t, gid_t) __KERNEL(lchown);
int fchownat(int, const char*, uid_t, gid_t, int) __KERNEL(fchownat);

int isatty(int) __KERNEL(isatty);
char* ttyname(int) __KERNEL(ttyname);
int ttyname_r(int, char*, size_t) __KERNEL(ttyname_r);
char* getlogin(void) __KERNEL(getlogin);
int getlogin_r(char*, size_t) __KERNEL(getlogin_r);
int gethostname(char*, size_t) __KERNEL(gethostname);
int sethostname(const char*, size_t) __KERNEL(sethostname);
int getdomainname(char*, size_t) __KERNEL(getdomainname);
int setdomainname(const char*, size_t) __KERNEL(setdomainname);
long gethostid(void) __KERNEL(gethostid);
int sethostid(long) __KERNEL(sethostid);
char* getusershell(void) __KERNEL(getusershell);
void setusershell(void) __KERNEL(setusershell);
void endusershell(void) __KERNEL(endusershell);
char* crypt(const char*, const char*) __KERNEL(crypt);
char* getpass(const char*) __KERNEL(getpass);
int daemon(int, int) __KERNEL(daemon);
int vhangup(void) __KERNEL(vhangup);
int acct(const char*) __KERNEL(acct);

unsigned sleep(unsigned) __KERNEL(sleep);
int usleep(useconds_t) __KERNEL(usleep);
unsigned alarm(unsigned) __KERNEL(alarm);
useconds_t ualarm(useconds_t, useconds_t) __KERNEL(ualarm);
int pause(void) __KERNEL(pause);
int nice(int) __KERNEL(nice);

long sysconf(int) __KERNEL(sysconf);
long pathconf(const char*, int) __KERNEL(pathconf);
long fpathconf(int, int) __KERNEL(fpathconf);
size_t confstr(int, char*, size_t) __KERNEL(confstr);
int getpagesize(void) __KERNEL(getpagesize);
int getdtablesize(void) __KERNEL(getdtablesize);
void swab(const void* __restrict, void* __restrict, ssize_t) __KERNEL(swab);

__END_DECLS
