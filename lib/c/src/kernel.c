/*
 * Every function lib/c's headers declare that the kernel implements, but
 * those the headers define __INLINE: a call of the kernel's import of the
 * same name, __kernel_<name> here. KERNEL declares the import and defines
 * the function, KERNEL_VOID and KERNEL_NORETURN those returning nothing;
 * a variadic function passes its va_list on, the pointer to its variable
 * arguments that the import took when the function was the import.
 */
#include <arpa/inet.h>
#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <glob.h>
#include <grp.h>
#include <inttypes.h>
#include <langinfo.h>
#include <libgen.h>
#include <locale.h>
#include <malloc.h>
#include <net/if.h>
#include <netdb.h>
#include <poll.h>
#include <pty.h>
#include <pwd.h>
#include <regex.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <syslog.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>
#include <wchar.h>
#include <wctype.h>

#define KERNEL(ret, name, params, args) \
    ret __kernel_##name params __KERNEL(name); \
    ret name params { \
        return __kernel_##name args; \
    }

#define KERNEL_VOID(name, params, args) \
    void __kernel_##name params __KERNEL(name); \
    void name params { \
        __kernel_##name args; \
    }

#define KERNEL_NORETURN(name, params, args) \
    __NORETURN void __kernel_##name params __KERNEL(name); \
    __NORETURN void name params { \
        __kernel_##name args; \
    }

/* arpa/inet.h */
KERNEL(in_addr_t, inet_addr, (const char* a), (a))
KERNEL(in_addr_t, inet_network, (const char* a), (a))
KERNEL(int, inet_aton, (const char* a, struct in_addr* b), (a, b))
KERNEL(char*, inet_ntoa, (struct in_addr a), (a))
KERNEL(int, inet_pton, (int a, const char* __restrict b, void* __restrict c), (a, b, c))
KERNEL(const char*, inet_ntop, (int a, const void* __restrict b, char* __restrict c, socklen_t d), (a, b, c, d))
KERNEL(struct in_addr, inet_makeaddr, (in_addr_t a, in_addr_t b), (a, b))
KERNEL(in_addr_t, inet_lnaof, (struct in_addr a), (a))
KERNEL(in_addr_t, inet_netof, (struct in_addr a), (a))

/* assert.h */
KERNEL_NORETURN(__assert_fail, (const char* a, const char* b, int c, const char* d), (a, b, c, d))

/* ctype.h */
KERNEL(int, isalnum, (int a), (a))
KERNEL(int, isalpha, (int a), (a))
KERNEL(int, isascii, (int a), (a))
KERNEL(int, isblank, (int a), (a))
KERNEL(int, iscntrl, (int a), (a))
KERNEL(int, isdigit, (int a), (a))
KERNEL(int, isgraph, (int a), (a))
KERNEL(int, islower, (int a), (a))
KERNEL(int, isprint, (int a), (a))
KERNEL(int, ispunct, (int a), (a))
KERNEL(int, isspace, (int a), (a))
KERNEL(int, isupper, (int a), (a))
KERNEL(int, isxdigit, (int a), (a))
KERNEL(int, tolower, (int a), (a))
KERNEL(int, toupper, (int a), (a))
KERNEL(int, toascii, (int a), (a))

/* dirent.h */
KERNEL(DIR*, opendir, (const char* a), (a))
KERNEL(DIR*, fdopendir, (int a), (a))
KERNEL(int, closedir, (DIR* a), (a))
KERNEL(struct dirent*, readdir, (DIR* a), (a))
KERNEL(int, readdir_r, (DIR* __restrict a, struct dirent* __restrict b, struct dirent** __restrict c), (a, b, c))
KERNEL_VOID(rewinddir, (DIR* a), (a))
KERNEL_VOID(seekdir, (DIR* a, long b), (a, b))
KERNEL(long, telldir, (DIR* a), (a))
KERNEL(int, dirfd, (DIR* a), (a))
KERNEL(int, alphasort, (const struct dirent** a, const struct dirent** b), (a, b))
KERNEL(int, versionsort, (const struct dirent** a, const struct dirent** b), (a, b))

/* fcntl.h */
KERNEL(int, creat, (const char* a, mode_t b), (a, b))
KERNEL(int, posix_fadvise, (int a, off_t b, off_t c, int d), (a, b, c, d))
KERNEL(int, posix_fallocate, (int a, off_t b, off_t c), (a, b, c))

/* fnmatch.h */
KERNEL(int, fnmatch, (const char* a, const char* b, int c), (a, b, c))

/* glob.h */
KERNEL(int, glob, (const char* __restrict a, int b, int (*c)(const char*, int), glob_t* __restrict d), (a, b, c, d))
KERNEL_VOID(globfree, (glob_t* a), (a))

/* grp.h */
KERNEL(struct group*, getgrnam, (const char* a), (a))
KERNEL(struct group*, getgrgid, (gid_t a), (a))
KERNEL(int, getgrnam_r, (const char* a, struct group* b, char* c, size_t d, struct group** e), (a, b, c, d, e))
KERNEL(int, getgrgid_r, (gid_t a, struct group* b, char* c, size_t d, struct group** e), (a, b, c, d, e))
KERNEL(struct group*, getgrent, (void), ())
KERNEL_VOID(setgrent, (void), ())
KERNEL_VOID(endgrent, (void), ())
KERNEL(int, getgrouplist, (const char* a, gid_t b, gid_t* c, int* d), (a, b, c, d))
KERNEL(int, initgroups, (const char* a, gid_t b), (a, b))
KERNEL(int, setgroups, (size_t a, const gid_t* b), (a, b))

/* inttypes.h */
KERNEL(intmax_t, imaxabs, (intmax_t a), (a))
KERNEL(imaxdiv_t, imaxdiv, (intmax_t a, intmax_t b), (a, b))

/* langinfo.h */
KERNEL(char*, nl_langinfo, (nl_item a), (a))

/* libgen.h */
KERNEL(char*, basename, (char* a), (a))
KERNEL(char*, dirname, (char* a), (a))

/* locale.h */
KERNEL(char*, setlocale, (int a, const char* b), (a, b))
KERNEL(struct lconv*, localeconv, (void), ())

/* malloc.h */
KERNEL(void*, memalign, (size_t a, size_t b), (a, b))
KERNEL(size_t, malloc_usable_size, (void* a), (a))
KERNEL(int, mallopt, (int a, int b), (a, b))
KERNEL(int, malloc_trim, (size_t a), (a))

/* net/if.h */
KERNEL(unsigned, if_nametoindex, (const char* a), (a))
KERNEL(char*, if_indextoname, (unsigned a, char* b), (a, b))
KERNEL(struct if_nameindex*, if_nameindex, (void), ())
KERNEL_VOID(if_freenameindex, (struct if_nameindex* a), (a))

/* netdb.h */
KERNEL(int, getaddrinfo, (const char* __restrict a, const char* __restrict b, const struct addrinfo* __restrict c, struct addrinfo** __restrict d), (a, b, c, d))
KERNEL_VOID(freeaddrinfo, (struct addrinfo* a), (a))
KERNEL(const char*, gai_strerror, (int a), (a))
KERNEL(int, getnameinfo, (const struct sockaddr* __restrict a, socklen_t b, char* __restrict c, socklen_t d, char* __restrict e, socklen_t f, int g), (a, b, c, d, e, f, g))
KERNEL(struct hostent*, gethostbyname, (const char* a), (a))
KERNEL(struct hostent*, gethostbyname2, (const char* a, int b), (a, b))
KERNEL(struct hostent*, gethostbyaddr, (const void* a, socklen_t b, int c), (a, b, c))
KERNEL_VOID(sethostent, (int a), (a))
KERNEL_VOID(endhostent, (void), ())
KERNEL(struct hostent*, gethostent, (void), ())
KERNEL(struct servent*, getservbyname, (const char* a, const char* b), (a, b))
KERNEL(struct servent*, getservbyport, (int a, const char* b), (a, b))
KERNEL(struct protoent*, getprotobyname, (const char* a), (a))
KERNEL(struct protoent*, getprotobynumber, (int a), (a))
KERNEL(struct netent*, getnetbyname, (const char* a), (a))
KERNEL(struct netent*, getnetbyaddr, (uint32_t a, int b), (a, b))
KERNEL_VOID(herror, (const char* a), (a))
KERNEL(const char*, hstrerror, (int a), (a))

/* poll.h */
KERNEL(int, poll, (struct pollfd* a, nfds_t b, int c), (a, b, c))
KERNEL(int, ppoll, (struct pollfd* a, nfds_t b, const struct timespec* c, const sigset_t* d), (a, b, c, d))

/* pty.h */
KERNEL(int, openpty, (int* a, int* b, char* c, const struct termios* d, const struct winsize* e), (a, b, c, d, e))

/* pwd.h */
KERNEL(struct passwd*, getpwnam, (const char* a), (a))
KERNEL(struct passwd*, getpwuid, (uid_t a), (a))
KERNEL(int, getpwnam_r, (const char* a, struct passwd* b, char* c, size_t d, struct passwd** e), (a, b, c, d, e))
KERNEL(int, getpwuid_r, (uid_t a, struct passwd* b, char* c, size_t d, struct passwd** e), (a, b, c, d, e))
KERNEL(struct passwd*, getpwent, (void), ())
KERNEL_VOID(setpwent, (void), ())
KERNEL_VOID(endpwent, (void), ())

/* regex.h */
KERNEL(int, regcomp, (regex_t* __restrict a, const char* __restrict b, int c), (a, b, c))
KERNEL(int, regexec, (const regex_t* __restrict a, const char* __restrict b, size_t c, regmatch_t* __restrict d, int e), (a, b, c, d, e))
KERNEL_VOID(regfree, (regex_t* a), (a))
KERNEL(size_t, regerror, (int a, const regex_t* __restrict b, char* __restrict c, size_t d), (a, b, c, d))

/* sched.h */
KERNEL(int, sched_yield, (void), ())
KERNEL(int, sched_get_priority_max, (int a), (a))
KERNEL(int, sched_get_priority_min, (int a), (a))
KERNEL(int, sched_getparam, (pid_t a, struct sched_param* b), (a, b))
KERNEL(int, sched_setparam, (pid_t a, const struct sched_param* b), (a, b))
KERNEL(int, sched_getscheduler, (pid_t a), (a))
KERNEL(int, sched_setscheduler, (pid_t a, int b, const struct sched_param* c), (a, b, c))
KERNEL(int, sched_rr_get_interval, (pid_t a, struct timespec* b), (a, b))

/* setjmp.h */
KERNEL_NORETURN(longjmp, (jmp_buf a, int b), (a, b))
KERNEL_NORETURN(_longjmp, (jmp_buf a, int b), (a, b))
KERNEL_NORETURN(siglongjmp, (sigjmp_buf a, int b), (a, b))

/* signal.h */
KERNEL(sighandler_t, signal, (int a, sighandler_t b), (a, b))
KERNEL(sighandler_t, bsd_signal, (int a, sighandler_t b), (a, b))
KERNEL(int, sigaction, (int a, const struct sigaction* __restrict b, struct sigaction* __restrict c), (a, b, c))
KERNEL(int, sigprocmask, (int a, const sigset_t* __restrict b, sigset_t* __restrict c), (a, b, c))
KERNEL(int, pthread_sigmask, (int a, const sigset_t* __restrict b, sigset_t* __restrict c), (a, b, c))
KERNEL(int, sigsuspend, (const sigset_t* a), (a))
KERNEL(int, sigpending, (sigset_t* a), (a))
KERNEL(int, sigwait, (const sigset_t* __restrict a, int* __restrict b), (a, b))
KERNEL(int, sigwaitinfo, (const sigset_t* __restrict a, siginfo_t* __restrict b), (a, b))
KERNEL(int, sigtimedwait, (const sigset_t* __restrict a, siginfo_t* __restrict b, const struct timespec* __restrict c), (a, b, c))
KERNEL(int, sigqueue, (pid_t a, int b, union sigval c), (a, b, c))
KERNEL(int, siginterrupt, (int a, int b), (a, b))
KERNEL(int, sigaltstack, (const stack_t* __restrict a, stack_t* __restrict b), (a, b))
KERNEL(int, sigemptyset, (sigset_t* a), (a))
KERNEL(int, sigfillset, (sigset_t* a), (a))
KERNEL(int, sigaddset, (sigset_t* a, int b), (a, b))
KERNEL(int, sigdelset, (sigset_t* a, int b), (a, b))
KERNEL(int, sigismember, (const sigset_t* a, int b), (a, b))
KERNEL(int, sigisemptyset, (const sigset_t* a), (a))
KERNEL(int, sigorset, (sigset_t* a, const sigset_t* b, const sigset_t* c), (a, b, c))
KERNEL(int, sigandset, (sigset_t* a, const sigset_t* b, const sigset_t* c), (a, b, c))
KERNEL(int, kill, (pid_t a, int b), (a, b))
KERNEL(int, killpg, (pid_t a, int b), (a, b))
KERNEL(int, raise, (int a), (a))
KERNEL_VOID(psignal, (int a, const char* b), (a, b))
KERNEL_VOID(psiginfo, (const siginfo_t* a, const char* b), (a, b))

/* stdio.h */
KERNEL(int, remove, (const char* a), (a))
KERNEL(int, rename, (const char* a, const char* b), (a, b))
KERNEL(int, renameat, (int a, const char* b, int c, const char* d), (a, b, c, d))
KERNEL(char*, ctermid, (char* a), (a))

/* stdlib.h */
KERNEL(void*, reallocarray, (void* a, size_t b, size_t c), (a, b, c))
KERNEL(void*, aligned_alloc, (size_t a, size_t b), (a, b))
KERNEL(int, posix_memalign, (void** a, size_t b, size_t c), (a, b, c))
KERNEL(void*, valloc, (size_t a), (a))
KERNEL_NORETURN(_Exit, (int a), (a))
KERNEL_NORETURN(abort, (void), ())
KERNEL(char*, getenv, (const char* a), (a))
KERNEL(char*, secure_getenv, (const char* a), (a))
KERNEL(int, setenv, (const char* a, const char* b, int c), (a, b, c))
KERNEL(int, unsetenv, (const char* a), (a))
KERNEL(int, putenv, (char* a), (a))
KERNEL(int, clearenv, (void), ())
KERNEL(int, system, (const char* a), (a))
KERNEL(char*, realpath, (const char* __restrict a, char* __restrict b), (a, b))
KERNEL(char*, canonicalize_file_name, (const char* a), (a))
KERNEL(char*, mktemp, (char* a), (a))
KERNEL(int, mkstemp, (char* a), (a))
KERNEL(int, mkostemp, (char* a, int b), (a, b))
KERNEL(int, mkstemps, (char* a, int b), (a, b))
KERNEL(char*, mkdtemp, (char* a), (a))
KERNEL(int, abs, (int a), (a))
KERNEL(long, labs, (long a), (a))
KERNEL(long long, llabs, (long long a), (a))
KERNEL(div_t, div, (int a, int b), (a, b))
KERNEL(ldiv_t, ldiv, (long a, long b), (a, b))
KERNEL(lldiv_t, lldiv, (long long a, long long b), (a, b))
KERNEL(int, rand, (void), ())
KERNEL_VOID(srand, (unsigned a), (a))
KERNEL(int, rand_r, (unsigned* a), (a))
KERNEL(long, random, (void), ())
KERNEL_VOID(srandom, (unsigned a), (a))
KERNEL(char*, initstate, (unsigned a, char* b, size_t c), (a, b, c))
KERNEL(char*, setstate, (char* a), (a))
KERNEL(int, posix_openpt, (int a), (a))
KERNEL(int, grantpt, (int a), (a))
KERNEL(int, unlockpt, (int a), (a))
KERNEL(char*, ptsname, (int a), (a))
KERNEL(int, ptsname_r, (int a, char* b, size_t c), (a, b, c))
KERNEL(int, getloadavg, (double* a, int b), (a, b))
KERNEL(int, getsubopt, (char** a, char* const* b, char** c), (a, b, c))

/* string.h */
KERNEL(void*, memrchr, (const void* a, int b, size_t c), (a, b, c))
KERNEL(void*, memmem, (const void* a, size_t b, const void* c, size_t d), (a, b, c, d))
KERNEL(void*, mempcpy, (void* __restrict a, const void* __restrict b, size_t c), (a, b, c))
KERNEL(void*, memccpy, (void* __restrict a, const void* __restrict b, int c, size_t d), (a, b, c, d))
KERNEL(void*, rawmemchr, (const void* a, int b), (a, b))
KERNEL(size_t, strnlen, (const char* a, size_t b), (a, b))
KERNEL(char*, strncpy, (char* __restrict a, const char* __restrict b, size_t c), (a, b, c))
KERNEL(char*, stpcpy, (char* __restrict a, const char* __restrict b), (a, b))
KERNEL(char*, stpncpy, (char* __restrict a, const char* __restrict b, size_t c), (a, b, c))
KERNEL(char*, strncat, (char* __restrict a, const char* __restrict b, size_t c), (a, b, c))
KERNEL(size_t, strlcpy, (char* a, const char* b, size_t c), (a, b, c))
KERNEL(size_t, strlcat, (char* a, const char* b, size_t c), (a, b, c))
KERNEL(int, strncmp, (const char* a, const char* b, size_t c), (a, b, c))
KERNEL(int, strcoll, (const char* a, const char* b), (a, b))
KERNEL(size_t, strxfrm, (char* __restrict a, const char* __restrict b, size_t c), (a, b, c))
KERNEL(int, strverscmp, (const char* a, const char* b), (a, b))
KERNEL(char*, strchr, (const char* a, int b), (a, b))
KERNEL(char*, strrchr, (const char* a, int b), (a, b))
KERNEL(char*, strchrnul, (const char* a, int b), (a, b))
KERNEL(char*, strstr, (const char* a, const char* b), (a, b))
KERNEL(char*, strcasestr, (const char* a, const char* b), (a, b))
KERNEL(char*, strpbrk, (const char* a, const char* b), (a, b))
KERNEL(char*, strtok, (char* __restrict a, const char* __restrict b), (a, b))
KERNEL(char*, strtok_r, (char* __restrict a, const char* __restrict b, char** __restrict c), (a, b, c))
KERNEL(char*, strsep, (char** a, const char* b), (a, b))
KERNEL(char*, strdup, (const char* a), (a))
KERNEL(char*, strndup, (const char* a, size_t b), (a, b))
KERNEL(char*, strerror, (int a), (a))
KERNEL(int, strerror_r, (int a, char* b, size_t c), (a, b, c))
KERNEL(char*, strsignal, (int a), (a))
KERNEL_VOID(explicit_bzero, (void* a, size_t b), (a, b))

/* strings.h */
KERNEL_VOID(bcopy, (const void* a, void* b, size_t c), (a, b, c))
KERNEL_VOID(bzero, (void* a, size_t b), (a, b))
KERNEL(char*, index, (const char* a, int b), (a, b))
KERNEL(char*, rindex, (const char* a, int b), (a, b))
KERNEL(int, ffs, (int a), (a))
KERNEL(int, ffsl, (long a), (a))
KERNEL(int, ffsll, (long long a), (a))
KERNEL(int, strcasecmp, (const char* a, const char* b), (a, b))
KERNEL(int, strncasecmp, (const char* a, const char* b, size_t c), (a, b, c))

/* sys/file.h */
KERNEL(int, flock, (int a, int b), (a, b))

/* sys/mman.h */
KERNEL(void*, mmap, (void* a, size_t b, int c, int d, int e, off_t f), (a, b, c, d, e, f))
KERNEL(int, munmap, (void* a, size_t b), (a, b))
KERNEL(int, mprotect, (void* a, size_t b, int c), (a, b, c))
KERNEL(int, msync, (void* a, size_t b, int c), (a, b, c))
KERNEL(int, madvise, (void* a, size_t b, int c), (a, b, c))
KERNEL(int, mlock, (const void* a, size_t b), (a, b))
KERNEL(int, munlock, (const void* a, size_t b), (a, b))
KERNEL(int, mlockall, (int a), (a))
KERNEL(int, munlockall, (void), ())

/* sys/resource.h */
KERNEL(int, getrlimit, (int a, struct rlimit* b), (a, b))
KERNEL(int, setrlimit, (int a, const struct rlimit* b), (a, b))
KERNEL(int, prlimit, (pid_t a, int b, const struct rlimit* c, struct rlimit* d), (a, b, c, d))
KERNEL(int, getrusage, (int a, struct rusage* b), (a, b))
KERNEL(int, getpriority, (int a, id_t b), (a, b))
KERNEL(int, setpriority, (int a, id_t b, int c), (a, b, c))

/* sys/select.h */
KERNEL(int, select, (int a, fd_set* __restrict b, fd_set* __restrict c, fd_set* __restrict d, struct timeval* __restrict e), (a, b, c, d, e))
KERNEL(int, pselect, (int a, fd_set* __restrict b, fd_set* __restrict c, fd_set* __restrict d, const struct timespec* __restrict e, const sigset_t* __restrict f), (a, b, c, d, e, f))

/* sys/socket.h */
KERNEL(int, socket, (int a, int b, int c), (a, b, c))
KERNEL(int, socketpair, (int a, int b, int c, int d[2]), (a, b, c, d))
KERNEL(int, bind, (int a, const struct sockaddr* b, socklen_t c), (a, b, c))
KERNEL(int, connect, (int a, const struct sockaddr* b, socklen_t c), (a, b, c))
KERNEL(int, listen, (int a, int b), (a, b))
KERNEL(int, accept, (int a, struct sockaddr* __restrict b, socklen_t* __restrict c), (a, b, c))
KERNEL(int, accept4, (int a, struct sockaddr* __restrict b, socklen_t* __restrict c, int d), (a, b, c, d))
KERNEL(int, getsockname, (int a, struct sockaddr* __restrict b, socklen_t* __restrict c), (a, b, c))
KERNEL(int, getpeername, (int a, struct sockaddr* __restrict b, socklen_t* __restrict c), (a, b, c))
KERNEL(int, getsockopt, (int a, int b, int c, void* __restrict d, socklen_t* __restrict e), (a, b, c, d, e))
KERNEL(int, setsockopt, (int a, int b, int c, const void* d, socklen_t e), (a, b, c, d, e))
KERNEL(ssize_t, send, (int a, const void* b, size_t c, int d), (a, b, c, d))
KERNEL(ssize_t, sendto, (int a, const void* b, size_t c, int d, const struct sockaddr* e, socklen_t f), (a, b, c, d, e, f))
KERNEL(ssize_t, sendmsg, (int a, const struct msghdr* b, int c), (a, b, c))
KERNEL(ssize_t, recv, (int a, void* b, size_t c, int d), (a, b, c, d))
KERNEL(ssize_t, recvfrom, (int a, void* __restrict b, size_t c, int d, struct sockaddr* __restrict e, socklen_t* __restrict f), (a, b, c, d, e, f))
KERNEL(ssize_t, recvmsg, (int a, struct msghdr* b, int c), (a, b, c))
KERNEL(int, shutdown, (int a, int b), (a, b))
KERNEL(int, sockatmark, (int a), (a))

/* sys/stat.h */
KERNEL(int, stat, (const char* __restrict a, struct stat* __restrict b), (a, b))
KERNEL(int, fstat, (int a, struct stat* b), (a, b))
KERNEL(int, lstat, (const char* __restrict a, struct stat* __restrict b), (a, b))
KERNEL(int, fstatat, (int a, const char* __restrict b, struct stat* __restrict c, int d), (a, b, c, d))
KERNEL(int, chmod, (const char* a, mode_t b), (a, b))
KERNEL(int, fchmod, (int a, mode_t b), (a, b))
KERNEL(int, fchmodat, (int a, const char* b, mode_t c, int d), (a, b, c, d))
KERNEL(int, mkdir, (const char* a, mode_t b), (a, b))
KERNEL(int, mkdirat, (int a, const char* b, mode_t c), (a, b, c))
KERNEL(int, mkfifo, (const char* a, mode_t b), (a, b))
KERNEL(int, mkfifoat, (int a, const char* b, mode_t c), (a, b, c))
KERNEL(int, mknod, (const char* a, mode_t b, dev_t c), (a, b, c))
KERNEL(int, mknodat, (int a, const char* b, mode_t c, dev_t d), (a, b, c, d))
KERNEL(mode_t, umask, (mode_t a), (a))
KERNEL(int, utimensat, (int a, const char* b, const struct timespec c[2], int d), (a, b, c, d))
KERNEL(int, futimens, (int a, const struct timespec b[2]), (a, b))

/* sys/statfs.h */
KERNEL(int, statfs, (const char* a, struct statfs* b), (a, b))
KERNEL(int, fstatfs, (int a, struct statfs* b), (a, b))

/* sys/statvfs.h */
KERNEL(int, statvfs, (const char* __restrict a, struct statvfs* __restrict b), (a, b))
KERNEL(int, fstatvfs, (int a, struct statvfs* b), (a, b))

/* sys/sysinfo.h */
KERNEL(int, sysinfo, (struct sysinfo* a), (a))
KERNEL(int, get_nprocs_conf, (void), ())
KERNEL(int, get_nprocs, (void), ())
KERNEL(long, get_phys_pages, (void), ())
KERNEL(long, get_avphys_pages, (void), ())

/* sys/time.h */
KERNEL(int, gettimeofday, (struct timeval* __restrict a, void* __restrict b), (a, b))
KERNEL(int, settimeofday, (const struct timeval* a, const struct timezone* b), (a, b))
KERNEL(int, getitimer, (int a, struct itimerval* b), (a, b))
KERNEL(int, setitimer, (int a, const struct itimerval* __restrict b, struct itimerval* __restrict c), (a, b, c))
KERNEL(int, utimes, (const char* a, const struct timeval b[2]), (a, b))
KERNEL(int, lutimes, (const char* a, const struct timeval b[2]), (a, b))
KERNEL(int, futimes, (int a, const struct timeval b[2]), (a, b))
KERNEL(int, adjtime, (const struct timeval* a, struct timeval* b), (a, b))

/* sys/times.h */
KERNEL(clock_t, times, (struct tms* a), (a))

/* sys/uio.h */
KERNEL(ssize_t, readv, (int a, const struct iovec* b, int c), (a, b, c))
KERNEL(ssize_t, writev, (int a, const struct iovec* b, int c), (a, b, c))
KERNEL(ssize_t, preadv, (int a, const struct iovec* b, int c, off_t d), (a, b, c, d))
KERNEL(ssize_t, pwritev, (int a, const struct iovec* b, int c, off_t d), (a, b, c, d))

/* sys/utsname.h */
KERNEL(int, uname, (struct utsname* a), (a))

/* sys/wait.h */
KERNEL(pid_t, wait, (int* a), (a))
KERNEL(pid_t, waitpid, (pid_t a, int* b, int c), (a, b, c))
KERNEL(pid_t, wait3, (int* a, int b, struct rusage* c), (a, b, c))
KERNEL(pid_t, wait4, (pid_t a, int* b, int c, struct rusage* d), (a, b, c, d))
KERNEL(int, waitid, (idtype_t a, id_t b, siginfo_t* c, int d), (a, b, c, d))

/* syslog.h */
KERNEL_VOID(openlog, (const char* a, int b, int c), (a, b, c))
KERNEL_VOID(closelog, (void), ())
KERNEL(int, setlogmask, (int a), (a))

/* termios.h */
KERNEL(int, tcgetattr, (int a, struct termios* b), (a, b))
KERNEL(int, tcsetattr, (int a, int b, const struct termios* c), (a, b, c))
KERNEL(int, tcsendbreak, (int a, int b), (a, b))
KERNEL(int, tcdrain, (int a), (a))
KERNEL(int, tcflush, (int a, int b), (a, b))
KERNEL(int, tcflow, (int a, int b), (a, b))
KERNEL(pid_t, tcgetsid, (int a), (a))
KERNEL(speed_t, cfgetospeed, (const struct termios* a), (a))
KERNEL(speed_t, cfgetispeed, (const struct termios* a), (a))
KERNEL(int, cfsetospeed, (struct termios* a, speed_t b), (a, b))
KERNEL(int, cfsetispeed, (struct termios* a, speed_t b), (a, b))
KERNEL(int, cfsetspeed, (struct termios* a, speed_t b), (a, b))
KERNEL_VOID(cfmakeraw, (struct termios* a), (a))

/* time.h */
KERNEL(time_t, time, (time_t* a), (a))
KERNEL(clock_t, clock, (void), ())
KERNEL(double, difftime, (time_t a, time_t b), (a, b))
KERNEL(time_t, mktime, (struct tm* a), (a))
KERNEL(time_t, timegm, (struct tm* a), (a))
KERNEL(struct tm*, gmtime, (const time_t* a), (a))
KERNEL(struct tm*, gmtime_r, (const time_t* __restrict a, struct tm* __restrict b), (a, b))
KERNEL(struct tm*, localtime, (const time_t* a), (a))
KERNEL(struct tm*, localtime_r, (const time_t* __restrict a, struct tm* __restrict b), (a, b))
KERNEL(char*, asctime, (const struct tm* a), (a))
KERNEL(char*, asctime_r, (const struct tm* __restrict a, char* __restrict b), (a, b))
KERNEL(char*, ctime, (const time_t* a), (a))
KERNEL(char*, ctime_r, (const time_t* a, char* b), (a, b))
KERNEL(size_t, strftime, (char* __restrict a, size_t b, const char* __restrict c, const struct tm* __restrict d), (a, b, c, d))
KERNEL(char*, strptime, (const char* __restrict a, const char* __restrict b, struct tm* __restrict c), (a, b, c))
KERNEL_VOID(tzset, (void), ())
KERNEL(int, nanosleep, (const struct timespec* a, struct timespec* b), (a, b))
KERNEL(int, clock_gettime, (clockid_t a, struct timespec* b), (a, b))
KERNEL(int, clock_settime, (clockid_t a, const struct timespec* b), (a, b))
KERNEL(int, clock_getres, (clockid_t a, struct timespec* b), (a, b))
KERNEL(int, clock_nanosleep, (clockid_t a, int b, const struct timespec* c, struct timespec* d), (a, b, c, d))
KERNEL(int, stime, (const time_t* a), (a))

/* unistd.h */
KERNEL(ssize_t, read, (int a, void* b, size_t c), (a, b, c))
KERNEL(ssize_t, write, (int a, const void* b, size_t c), (a, b, c))
KERNEL(ssize_t, pread, (int a, void* b, size_t c, off_t d), (a, b, c, d))
KERNEL(ssize_t, pwrite, (int a, const void* b, size_t c, off_t d), (a, b, c, d))
KERNEL(ssize_t, copy_file_range, (int a, off_t* b, int c, off_t* d, size_t e, unsigned f), (a, b, c, d, e, f))
KERNEL(off_t, lseek, (int a, off_t b, int c), (a, b, c))
KERNEL(int, close, (int a), (a))
KERNEL(int, dup, (int a), (a))
KERNEL(int, dup2, (int a, int b), (a, b))
KERNEL(int, dup3, (int a, int b, int c), (a, b, c))
KERNEL(int, pipe, (int a[2]), (a))
KERNEL(int, pipe2, (int a[2], int b), (a, b))
KERNEL(int, fsync, (int a), (a))
KERNEL(int, fdatasync, (int a), (a))
KERNEL_VOID(sync, (void), ())
KERNEL(int, syncfs, (int a), (a))
KERNEL(int, ftruncate, (int a, off_t b), (a, b))
KERNEL(int, truncate, (const char* a, off_t b), (a, b))
KERNEL(int, lockf, (int a, int b, off_t c), (a, b, c))
KERNEL(int, execve, (const char* a, char* const b[], char* const c[]), (a, b, c))
KERNEL(int, execv, (const char* a, char* const b[]), (a, b))
KERNEL(int, execvp, (const char* a, char* const b[]), (a, b))
KERNEL(int, execvpe, (const char* a, char* const b[], char* const c[]), (a, b, c))
KERNEL(int, fexecve, (int a, char* const b[], char* const c[]), (a, b, c))
KERNEL_NORETURN(_exit, (int a), (a))
KERNEL(pid_t, getpid, (void), ())
KERNEL(pid_t, getppid, (void), ())
KERNEL(pid_t, getpgrp, (void), ())
KERNEL(pid_t, getpgid, (pid_t a), (a))
KERNEL(int, setpgid, (pid_t a, pid_t b), (a, b))
KERNEL(int, setpgrp, (void), ())
KERNEL(pid_t, getsid, (pid_t a), (a))
KERNEL(pid_t, setsid, (void), ())
KERNEL(pid_t, tcgetpgrp, (int a), (a))
KERNEL(int, tcsetpgrp, (int a, pid_t b), (a, b))
KERNEL(uid_t, getuid, (void), ())
KERNEL(uid_t, geteuid, (void), ())
KERNEL(gid_t, getgid, (void), ())
KERNEL(gid_t, getegid, (void), ())
KERNEL(int, setuid, (uid_t a), (a))
KERNEL(int, seteuid, (uid_t a), (a))
KERNEL(int, setgid, (gid_t a), (a))
KERNEL(int, setegid, (gid_t a), (a))
KERNEL(int, setreuid, (uid_t a, uid_t b), (a, b))
KERNEL(int, setregid, (gid_t a, gid_t b), (a, b))
KERNEL(int, getresuid, (uid_t* a, uid_t* b, uid_t* c), (a, b, c))
KERNEL(int, getresgid, (gid_t* a, gid_t* b, gid_t* c), (a, b, c))
KERNEL(int, setresuid, (uid_t a, uid_t b, uid_t c), (a, b, c))
KERNEL(int, setresgid, (gid_t a, gid_t b, gid_t c), (a, b, c))
KERNEL(int, getgroups, (int a, gid_t b[]), (a, b))
KERNEL(int, access, (const char* a, int b), (a, b))
KERNEL(int, faccessat, (int a, const char* b, int c, int d), (a, b, c, d))
KERNEL(int, euidaccess, (const char* a, int b), (a, b))
KERNEL(int, eaccess, (const char* a, int b), (a, b))
KERNEL(int, chdir, (const char* a), (a))
KERNEL(int, fchdir, (int a), (a))
KERNEL(int, chroot, (const char* a), (a))
KERNEL(char*, getcwd, (char* a, size_t b), (a, b))
KERNEL(char*, get_current_dir_name, (void), ())
KERNEL(int, unlink, (const char* a), (a))
KERNEL(int, unlinkat, (int a, const char* b, int c), (a, b, c))
KERNEL(int, rmdir, (const char* a), (a))
KERNEL(int, link, (const char* a, const char* b), (a, b))
KERNEL(int, linkat, (int a, const char* b, int c, const char* d, int e), (a, b, c, d, e))
KERNEL(int, symlink, (const char* a, const char* b), (a, b))
KERNEL(int, symlinkat, (const char* a, int b, const char* c), (a, b, c))
KERNEL(ssize_t, readlink, (const char* __restrict a, char* __restrict b, size_t c), (a, b, c))
KERNEL(ssize_t, readlinkat, (int a, const char* __restrict b, char* __restrict c, size_t d), (a, b, c, d))
KERNEL(int, chown, (const char* a, uid_t b, gid_t c), (a, b, c))
KERNEL(int, fchown, (int a, uid_t b, gid_t c), (a, b, c))
KERNEL(int, lchown, (const char* a, uid_t b, gid_t c), (a, b, c))
KERNEL(int, fchownat, (int a, const char* b, uid_t c, gid_t d, int e), (a, b, c, d, e))
KERNEL(int, isatty, (int a), (a))
KERNEL(char*, ttyname, (int a), (a))
KERNEL(int, ttyname_r, (int a, char* b, size_t c), (a, b, c))
KERNEL(char*, getlogin, (void), ())
KERNEL(int, getlogin_r, (char* a, size_t b), (a, b))
KERNEL(int, gethostname, (char* a, size_t b), (a, b))
KERNEL(int, sethostname, (const char* a, size_t b), (a, b))
KERNEL(int, getdomainname, (char* a, size_t b), (a, b))
KERNEL(int, setdomainname, (const char* a, size_t b), (a, b))
KERNEL(long, gethostid, (void), ())
KERNEL(int, sethostid, (long a), (a))
KERNEL(char*, getusershell, (void), ())
KERNEL_VOID(setusershell, (void), ())
KERNEL_VOID(endusershell, (void), ())
KERNEL(char*, crypt, (const char* a, const char* b), (a, b))
KERNEL(char*, getpass, (const char* a), (a))
KERNEL(int, daemon, (int a, int b), (a, b))
KERNEL(int, vhangup, (void), ())
KERNEL(int, acct, (const char* a), (a))
KERNEL(unsigned, sleep, (unsigned a), (a))
KERNEL(int, usleep, (useconds_t a), (a))
KERNEL(unsigned, alarm, (unsigned a), (a))
KERNEL(useconds_t, ualarm, (useconds_t a, useconds_t b), (a, b))
KERNEL(int, pause, (void), ())
KERNEL(int, nice, (int a), (a))
KERNEL(long, sysconf, (int a), (a))
KERNEL(long, pathconf, (const char* a, int b), (a, b))
KERNEL(long, fpathconf, (int a, int b), (a, b))
KERNEL(size_t, confstr, (int a, char* b, size_t c), (a, b, c))
KERNEL(int, getpagesize, (void), ())
KERNEL(int, getdtablesize, (void), ())
KERNEL_VOID(swab, (const void* __restrict a, void* __restrict b, ssize_t c), (a, b, c))

/* utime.h */
KERNEL(int, utime, (const char* a, const struct utimbuf* b), (a, b))

/* wchar.h */
KERNEL(size_t, wcslen, (const wchar_t* a), (a))
KERNEL(size_t, wcsnlen, (const wchar_t* a, size_t b), (a, b))
KERNEL(wchar_t*, wcscpy, (wchar_t* __restrict a, const wchar_t* __restrict b), (a, b))
KERNEL(wchar_t*, wcsncpy, (wchar_t* __restrict a, const wchar_t* __restrict b, size_t c), (a, b, c))
KERNEL(wchar_t*, wcscat, (wchar_t* __restrict a, const wchar_t* __restrict b), (a, b))
KERNEL(int, wcscmp, (const wchar_t* a, const wchar_t* b), (a, b))
KERNEL(int, wcsncmp, (const wchar_t* a, const wchar_t* b, size_t c), (a, b, c))
KERNEL(int, wcscoll, (const wchar_t* a, const wchar_t* b), (a, b))
KERNEL(wchar_t*, wcschr, (const wchar_t* a, wchar_t b), (a, b))
KERNEL(wchar_t*, wcsrchr, (const wchar_t* a, wchar_t b), (a, b))
KERNEL(wchar_t*, wcsstr, (const wchar_t* __restrict a, const wchar_t* __restrict b), (a, b))
KERNEL(wchar_t*, wcspbrk, (const wchar_t* a, const wchar_t* b), (a, b))
KERNEL(size_t, wcsspn, (const wchar_t* a, const wchar_t* b), (a, b))
KERNEL(size_t, wcscspn, (const wchar_t* a, const wchar_t* b), (a, b))
KERNEL(wchar_t*, wcstok, (wchar_t* __restrict a, const wchar_t* __restrict b, wchar_t** __restrict c), (a, b, c))
KERNEL(size_t, wcsxfrm, (wchar_t* __restrict a, const wchar_t* __restrict b, size_t c), (a, b, c))
KERNEL(size_t, wcsftime, (wchar_t* __restrict a, size_t b, const wchar_t* __restrict c, const struct tm* __restrict d), (a, b, c, d))
KERNEL(wchar_t*, wcsdup, (const wchar_t* a), (a))
KERNEL(wchar_t*, wmemchr, (const wchar_t* a, wchar_t b, size_t c), (a, b, c))
KERNEL(int, wmemcmp, (const wchar_t* a, const wchar_t* b, size_t c), (a, b, c))
KERNEL(wchar_t*, wmemcpy, (wchar_t* __restrict a, const wchar_t* __restrict b, size_t c), (a, b, c))
KERNEL(wchar_t*, wmemmove, (wchar_t* a, const wchar_t* b, size_t c), (a, b, c))
KERNEL(wchar_t*, wmemset, (wchar_t* a, wchar_t b, size_t c), (a, b, c))
KERNEL(int, wcwidth, (wchar_t a), (a))
KERNEL(int, wcswidth, (const wchar_t* a, size_t b), (a, b))

/* wctype.h */
KERNEL(int, iswalnum, (wint_t a), (a))
KERNEL(int, iswalpha, (wint_t a), (a))
KERNEL(int, iswblank, (wint_t a), (a))
KERNEL(int, iswcntrl, (wint_t a), (a))
KERNEL(int, iswdigit, (wint_t a), (a))
KERNEL(int, iswgraph, (wint_t a), (a))
KERNEL(int, iswlower, (wint_t a), (a))
KERNEL(int, iswprint, (wint_t a), (a))
KERNEL(int, iswpunct, (wint_t a), (a))
KERNEL(int, iswspace, (wint_t a), (a))
KERNEL(int, iswupper, (wint_t a), (a))
KERNEL(int, iswxdigit, (wint_t a), (a))
KERNEL(int, iswctype, (wint_t a, wctype_t b), (a, b))
KERNEL(wctype_t, wctype, (const char* a), (a))
KERNEL(wint_t, towlower, (wint_t a), (a))
KERNEL(wint_t, towupper, (wint_t a), (a))
KERNEL(wint_t, towctrans, (wint_t a, wctrans_t b), (a, b))
KERNEL(wctrans_t, wctrans, (const char* a), (a))

/* The variadic ones: the import takes the va_list. */
int __kernel_open(const char*, int, va_list) __KERNEL(open);
int __kernel_openat(int, const char*, int, va_list) __KERNEL(openat);
int __kernel_fcntl(int, int, va_list) __KERNEL(fcntl);
int __kernel_ioctl(int, unsigned long, va_list) __KERNEL(ioctl);
int __kernel_prctl(int, va_list) __KERNEL(prctl);
int __kernel_execl(const char*, const char*, va_list) __KERNEL(execl);
int __kernel_execlp(const char*, const char*, va_list) __KERNEL(execlp);
int __kernel_execle(const char*, const char*, va_list) __KERNEL(execle);

int open(const char* path, int flags, ...) {
    va_list ap;
    va_start(ap, flags);
    int r = __kernel_open(path, flags, ap);
    va_end(ap);
    return r;
}

int openat(int dirfd, const char* path, int flags, ...) {
    va_list ap;
    va_start(ap, flags);
    int r = __kernel_openat(dirfd, path, flags, ap);
    va_end(ap);
    return r;
}

int fcntl(int fd, int cmd, ...) {
    va_list ap;
    va_start(ap, cmd);
    int r = __kernel_fcntl(fd, cmd, ap);
    va_end(ap);
    return r;
}

int ioctl(int fd, unsigned long request, ...) {
    va_list ap;
    va_start(ap, request);
    int r = __kernel_ioctl(fd, request, ap);
    va_end(ap);
    return r;
}

int prctl(int option, ...) {
    va_list ap;
    va_start(ap, option);
    int r = __kernel_prctl(option, ap);
    va_end(ap);
    return r;
}

int execl(const char* path, const char* arg, ...) {
    va_list ap;
    va_start(ap, arg);
    int r = __kernel_execl(path, arg, ap);
    va_end(ap);
    return r;
}

int execlp(const char* file, const char* arg, ...) {
    va_list ap;
    va_start(ap, arg);
    int r = __kernel_execlp(file, arg, ap);
    va_end(ap);
    return r;
}

int execle(const char* path, const char* arg, ...) {
    va_list ap;
    va_start(ap, arg);
    int r = __kernel_execle(path, arg, ap);
    va_end(ap);
    return r;
}
