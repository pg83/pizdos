/*
 * lib/c's functions with deterministic results, one line each, for
 * test_libc.py to compare: strings, wide strings, classes, locales, time,
 * the environment, random, the heap, files and directories, the user
 * databases, signals and the in-process parts.
 */
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fenv.h>
#include <glob.h>
#include <grp.h>
#include <langinfo.h>
#include <libgen.h>
#include <locale.h>
#include <malloc.h>
#include <mntent.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pwd.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

static void strings(void) {
    char text[] = "a,b,,c";
    char* parts[4] = {0};
    int n = 0;

    for (char* t = strtok(text, ","); t; t = strtok(0, ",")) {
        parts[n++] = t;
    }

    printf("strtok %d %s %s %s\n", n, parts[0], parts[1], parts[2]);

    char buf[8];
    size_t l = strlcpy(buf, "overflowing", sizeof buf);
    printf("strlcpy %zu %s %zu\n", l, buf, strlcat(buf, "xyz", sizeof buf));
    printf("memrchr %td memmem %td rawmemchr %td\n", (char*)memrchr("abcabc", 'b', 6) - "abcabc",
           (char*)memmem("hello world", 11, "o w", 3) - "hello world", (char*)rawmemchr("find", 'n') - "find");
    char path1[] = "/usr/lib/";
    char path2[] = "file.txt";
    printf("basename %s %s\n", basename(path1), basename(path2));
    char err[32];
    printf("strerror_r %d %s", strerror_r(ENOENT, err, sizeof err), err);
    printf(" %d %s\n", strerror_r(ENOENT, err, 4), err);
    div_t d = div(-7, 2);
    lldiv_t ld = lldiv(1000000000000LL, 7);
    printf("div %d %d %lld %lld ffs %d %d\n", d.quot, d.rem, ld.quot, ld.rem, ffs(0x50), ffsll(1LL << 40));
    char copy[8];
    printf("memccpy %td swab", (char*)memccpy(copy, "abc:def", ':', 7) - copy);
    char swapped[4];
    swab("1234", swapped, 4);
    printf(" %.4s strcoll %d\n", swapped, strcoll("a", "b") < 0);
}

static void wide(void) {
    wchar_t w[32];
    wcscpy(w, L"héllo wörld");
    wcscat(w, L"!");
    printf("wcslen %zu wcschr %td wcsrchr %td wcsstr %td\n", wcslen(w), wcschr(w, L'ö') - w, wcsrchr(w, L'l') - w,
           wcsstr(w, L"wör") - w);
    printf("wcsspn %zu wcscspn %zu wcspbrk %td wcscmp %d %d\n", wcsspn(w, L"leh"), wcscspn(w, L" "), wcspbrk(w, L"wd") - w,
           wcscmp(L"abc", L"abd") < 0, wcsncmp(L"abc", L"abd", 2));
    wchar_t* save;
    wchar_t text[] = L"x y  z";
    int count = 0;

    for (wchar_t* t = wcstok(text, L" ", &save); t; t = wcstok(0, L" ", &save)) {
        count++;
    }

    printf("wcstok %d wcwidth %d %d %d %d wcswidth %d\n", count, wcwidth(L'a'), wcwidth(0x4e2d), wcwidth(0x301), wcwidth(7),
           wcswidth(L"a中b", 3));
    wchar_t* end;
    printf("wcstol %ld %ls wcstod %g\n", wcstol(L"  -123x", &end, 10), end, wcstod(L"2.5e3", 0));
    struct tm tm = {.tm_year = 120, .tm_mon = 1, .tm_mday = 29, .tm_hour = 13};
    wchar_t out[64];
    wcsftime(out, 64, L"%Y-%m-%d %H %a é", &tm);
    printf("wcsftime %ls\n", out);
    printf("wmem %td %d", wmemchr(L"abcd", L'c', 4) - L"abcd", wmemcmp(L"ab", L"ac", 2) < 0);
    wmemset(w, L'z', 3);
    w[3] = 0;
    printf(" %ls\n", w);
}

static void classes(void) {
    printf("ctype %d%d%d%d%d%d%d%d %c%c\n", isalpha('a'), isalpha(0xe9), isdigit('7'), isxdigit('F'), ispunct('!'), isspace('\t'),
           isupper('Q'), iscntrl(127), toupper('a'), tolower('Z'));
    printf("wctype %d%d%d%d%d%d %lc%lc %d\n", !!iswalpha(0xe9), !!iswalpha(0x4e2d), !!iswdigit(0x661), !!iswspace(0x3000),
           !!iswupper(0x410), !!iswlower(0x3b1), towupper(0xe9), towlower(0x416), !!iswctype(L'x', wctype("xdigit")));
    printf("towctrans %lc %lc\n", towctrans(L'a', wctrans("toupper")), towctrans(L'B', wctrans("tolower")));
}

static void locales(void) {
    printf("setlocale %s", setlocale(LC_ALL, 0));
    printf(" %s", setlocale(LC_CTYPE, "C.UTF-8"));
    printf(" %s", setlocale(LC_ALL, 0));
    printf(" %s", setlocale(LC_NUMERIC, "en_US.ISO-8859-1") ? "accepted" : "refused");
    printf(" %s\n", setlocale(LC_ALL, "C"));
    struct lconv* lc = localeconv();
    printf("localeconv [%s] [%s] %d\n", lc->decimal_point, lc->thousands_sep, lc->frac_digits);
    printf("langinfo %s %s %s %s %s\n", nl_langinfo(CODESET), nl_langinfo(DAY_2), nl_langinfo(ABMON_12), nl_langinfo(D_T_FMT),
           nl_langinfo(YESEXPR));
}

static void times(void) {
    time_t t = 1700000000;
    struct tm tm;
    gmtime_r(&t, &tm);
    char buf[32];
    printf("gmtime %d-%02d-%02d %02d:%02d %s yday %d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
           tm.tm_zone, tm.tm_yday);
    printf(" asctime %s", asctime_r(&tm, buf));
    struct tm in = {.tm_year = 70, .tm_mon = 0, .tm_mday = 32, .tm_hour = 25};
    printf("timegm %lld %d/%d %d\n", (long long)timegm(&in), in.tm_mon + 1, in.tm_mday, in.tm_hour);
    printf("difftime %g", difftime(10, 4));
    struct timespec res;
    printf(" clock_getres %d", clock_getres(CLOCK_MONOTONIC, &res));
    printf(" cpu %d", clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &res) == 0 && clock() >= 0);
    struct timespec nap = {0, 1000000};
    printf(" nanosleep %d\n", clock_nanosleep(CLOCK_MONOTONIC, 0, &nap, 0));
    setenv("TZ", "EST5EDT", 1);
    tzset();
    printf("tzset %s %s %ld %d\n", tzname[0], tzname[1], timezone, daylight);
    unsetenv("TZ");
}

static void environment(void) {
    printf("setenv %d %s", setenv("CHECK_A", "1", 0), getenv("CHECK_A"));
    printf(" %d %s", setenv("CHECK_A", "2", 0), getenv("CHECK_A"));
    printf(" %d %s", setenv("CHECK_A", "3", 1), getenv("CHECK_A"));
    printf(" %d %s", unsetenv("CHECK_A"), getenv("CHECK_A") ? "set" : "unset");
    errno = 0;
    printf(" %d %d\n", setenv("BAD=NAME", "x", 1), errno == EINVAL);
    char opts[] = "ro,size=10,mode";
    char* p = opts;
    char* keys[] = {"ro", "size", "mode", 0};
    char* value;
    printf("getsubopt");

    while (*p) {
        int i = getsubopt(&p, keys, &value);
        printf(" %d:%s", i, value ? value : "-");
    }

    printf("\n");
}

static void randoms(void) {
    srandom(1);
    long a = random();
    long b = random();
    char state[64];
    char* old = initstate(7, state, sizeof state);
    long c = random();
    setstate(old);
    srandom(1);
    printf("random %ld %ld %ld %d", a, b, c, random() == a);
    unsigned seed = 1;
    printf(" rand_r %d %d\n", rand_r(&seed), rand_r(&seed));
}

static void heap(void) {
    void* p = 0;
    int e = posix_memalign(&p, 4096, 100);
    void* q = aligned_alloc(256, 10);
    void* r = memalign(64, 1000);
    printf("memalign %d %d %d %d", e, (uintptr_t)p % 4096 == 0, (uintptr_t)q % 256 == 0, (uintptr_t)r % 64 == 0);
    printf(" usable %d", malloc_usable_size(r) >= 1000);
    free(p);
    free(q);
    free(r);
    int* a = reallocarray(0, 10, sizeof(int));
    a[9] = 1;
    errno = 0;
    printf(" reallocarray %d %d", reallocarray(a, SIZE_MAX / 2, 4) == 0, errno == ENOMEM);
    free(a);
    char* m = mmap(0, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    m[8191] = 1;
    printf(" mmap %d %d", madvise(m, 8192, MADV_DONTNEED), m[8191]);
    printf(" %d %d\n", mprotect(m, 4096, PROT_READ), munmap(m, 8192));
}

static void files(void) {
    mkdir("/tmp/d", 0755);
    int dir = open("/tmp/d", O_RDONLY | O_DIRECTORY);
    int fd = openat(dir, "f", O_CREAT | O_RDWR, 0644);
    printf("openat %d pwrite %zd pread", fd >= 0, pwrite(fd, "abcdef", 6, 0));
    char buf[8] = {0};
    pread(fd, buf, 3, 2);
    printf(" %s", buf);
    struct stat st;
    printf(" fstatat %d %lld", fstatat(dir, "f", &st, 0), (long long)st.st_size);
    printf(" faccessat %d", faccessat(dir, "f", R_OK, 0));
    printf(" renameat %d", renameat(dir, "f", dir, "g"));
    printf(" linkat %d", linkat(dir, "g", dir, "h", 0));
    printf(" symlinkat %d", symlinkat("g", dir, "s"));
    char link[8] = {0};
    printf(" readlinkat %zd %s", readlinkat(dir, "s", link, sizeof link), link);
    printf(" truncate %d", truncate("/tmp/d/g", 2));
    stat("/tmp/d/h", &st);
    printf(" %lld", (long long)st.st_size);
    printf(" unlinkat %d %d\n", unlinkat(dir, "h", 0), unlinkat(dir, "s", 0));
    printf("mkfifo %d", mkfifo("/tmp/d/p", 0600));
    stat("/tmp/d/p", &st);
    printf(" %d", S_ISFIFO(st.st_mode));
    struct statvfs vfs;
    printf(" statvfs %d %d", statvfs("/tmp", &vfs), vfs.f_bsize > 0);
    printf(" flock %d lockf %d %d\n", flock(fd, LOCK_EX), lockf(fd, F_LOCK, 0), lockf(fd, F_TEST, 0));
    close(fd);
    // A directory opened before its entries were made may not list them
    // until it is rewound: btrfs's do not.
    DIR* dd = fdopendir(dir);
    rewinddir(dd);
    int entries = 0;
    long at = 0;
    struct dirent* e;

    while ((e = readdir(dd))) {
        if (++entries == 2) {
            at = telldir(dd);
        }
    }

    seekdir(dd, at);
    int rest = 0;

    while (readdir(dd)) {
        rest++;
    }

    rewinddir(dd);
    struct dirent entry, *result;
    printf("dir %d %d %d", entries, rest, readdir_r(dd, &entry, &result) == 0 && result == &entry);
    closedir(dd);
    struct dirent** list;
    int n = scandir("/tmp/d", &list, 0, alphasort);
    printf(" scandir %d", n);

    for (int i = 0; i < n; i++) {
        printf(" %s", list[i]->d_name);
        free(list[i]);
    }

    free(list);
    glob_t g;
    printf(" glob %d", glob("/tmp/d/*", GLOB_MARK, 0, &g));

    for (size_t i = 0; i < g.gl_pathc; i++) {
        printf(" %s", g.gl_pathv[i]);
    }

    printf(" %d\n", glob("/tmp/d/none*", 0, 0, &g) == GLOB_NOMATCH);
    globfree(&g);
    char template[] = "/tmp/fileXXXXXXsuffix";
    int t = mkstemps(template, 6);
    printf("mkstemps %d %s", t >= 0, strstr(template, "suffix") ? "suffix" : "-");
    unlink(template);
    FILE* tmp = tmpfile();
    fputs("temporary", tmp);
    rewind(tmp);
    char back[16] = {0};
    fgets(back, sizeof back, tmp);
    printf(" tmpfile %s", back);
    fclose(tmp);
    char name[L_tmpnam];
    char* other = tempnam("/tmp", "pre");
    printf(" tmpnam %d tempnam %d\n", tmpnam(name) == name && strncmp(name, "/tmp/tmpnam_", 12) == 0,
           strncmp(other, "/tmp/pre_", 9) == 0);
    free(other);
}

static void users(void) {
    struct passwd* pw = getpwnam("user");
    printf("getpwnam %s %u %s", pw->pw_name, pw->pw_uid, pw->pw_dir);
    struct passwd entry, *result;
    char buf[256];
    printf(" getpwuid_r %d %s", getpwuid_r(0, &entry, buf, sizeof buf, &result), result->pw_name);
    printf(" %d", getpwuid_r(0, &entry, buf, 4, &result));
    setpwent();
    int count = 0;

    while (getpwent()) {
        count++;
    }

    endpwent();
    printf(" getpwent %d", count);
    struct group* gr = getgrnam("wheel");
    printf(" getgrnam %s %u %s %s", gr->gr_name, gr->gr_gid, gr->gr_mem[0], gr->gr_mem[1]);
    gid_t groups[8];
    int ngroups = 8;
    printf(" getgrouplist %d", getgrouplist("user", 100, groups, &ngroups));

    for (int i = 0; i < ngroups; i++) {
        printf(" %u", groups[i]);
    }

    FILE* f = fopen("/etc/passwd", "r");
    struct passwd* fp = fgetpwent(f);
    printf(" fgetpwent %s", fp->pw_name);
    fclose(f);
    f = fopen("/etc/group", "r");
    fgetgrent(f);
    struct group* fg = fgetgrent(f);
    printf(" fgetgrent %s %s\n", fg->gr_name, fg->gr_mem[0]);
    fclose(f);
    f = fopen("/tmp/mtab", "w+");
    struct mntent m = {"/dev/root", "/", "ext4", "rw,noatime", 0, 1};
    addmntent(f, &m);
    rewind(f);
    struct mntent* got = getmntent(f);
    printf("mntent %s %s %s %d %s\n", got->mnt_dir, got->mnt_type, got->mnt_opts, got->mnt_passno, hasmntopt(got, "noatime"));
    endmntent(f);
}

static volatile int caught;

static void handler(int sig) {
    caught = sig;
}

static void signals(void) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    printf("sigset %d %d %d", sigismember(&set, SIGUSR1), sigismember(&set, SIGUSR2), sigaddset(&set, 32));
    sigprocmask(SIG_BLOCK, &set, 0);
    kill(getpid(), SIGUSR1);
    sigset_t pending;
    sigpending(&pending);
    int sig = 0;
    printf(" pending %d sigwait %d %d", sigismember(&pending, SIGUSR1), sigwait(&set, &sig), sig);
    struct timespec zero = {0, 0};
    printf(" sigtimedwait %d %d", sigtimedwait(&set, 0, &zero), errno == EAGAIN);
    signal(SIGALRM, handler);
    alarm(1);
    printf(" alarm %d", alarm(0) == 1);
    struct itimerval it = {{0, 0}, {0, 20000}};
    setitimer(ITIMER_REAL, &it, 0);
    pause();
    printf(" setitimer %d", caught == SIGALRM);
    char stack[SIGSTKSZ];
    stack_t ss = {stack, 0, sizeof stack}, old;
    printf(" sigaltstack %d %d\n", sigaltstack(&ss, &old), old.ss_flags == SS_DISABLE);
    pid_t pid = fork();

    if (!pid) {
        _exit(5);
    }

    siginfo_t info;
    printf("waitid %d %d %d", waitid(P_PID, pid, &info, WEXITED | WNOWAIT), info.si_pid == pid, info.si_status);
    int status;
    printf(" wait4 %d %d\n", wait4(pid, &status, 0, 0) == pid, WEXITSTATUS(status));
}

static void network(void) {
    struct in_addr in;
    printf("inet %d %08x %08x %s", inet_aton("10.1.2", &in), in.s_addr, inet_addr("0x7f.1"), inet_ntoa(in));
    unsigned char six[16];
    char text[INET6_ADDRSTRLEN];
    printf(" %d %s %d %d\n", inet_pton(AF_INET6, "2001:db8::ff00:42:8329", six), inet_ntop(AF_INET6, six, text, sizeof text),
           inet_pton(AF_INET, "1.2.3.04", six), inet_pton(AF_INET, "1.2.3.4", six));
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM, .ai_flags = AI_CANONNAME};
    struct addrinfo* res;
    int e = getaddrinfo("box", "http", &hints, &res);
    printf("getaddrinfo %d", e);

    for (struct addrinfo* a = res; !e && a; a = a->ai_next) {
        char host[64], serv[16];
        getnameinfo(a->ai_addr, a->ai_addrlen, host, sizeof host, serv, sizeof serv, NI_NUMERICHOST | NI_NUMERICSERV);
        printf(" %d/%d %s:%s %s", a->ai_family, a->ai_protocol, host, serv, a->ai_canonname ? a->ai_canonname : "-");
    }

    freeaddrinfo(res);
    hints.ai_flags = AI_NUMERICHOST;
    printf(" %d %s\n", getaddrinfo("box", 0, &hints, &res), gai_strerror(EAI_NONAME));
    struct sockaddr_in sin = {.sin_family = AF_INET, .sin_port = htons(22)};
    inet_pton(AF_INET, "10.0.0.7", &sin.sin_addr);
    char host[64], serv[16];
    printf("getnameinfo %d %s %s", getnameinfo((struct sockaddr*)&sin, sizeof sin, host, sizeof host, serv, sizeof serv, 0), host, serv);
    struct servent* se = getservbyname("domain", "udp");
    struct protoent* pe = getprotobyname("udp");
    printf(" servent %s %d %s protoent %s %d", se->s_name, ntohs(se->s_port), se->s_proto, pe->p_name, pe->p_proto);
    struct hostent* he = gethostbyname("box");
    printf(" hostent %s %s %d\n", he->h_name, inet_ntoa(*(struct in_addr*)he->h_addr_list[0]),
           gethostbyname2("box", AF_UNIX) == 0 && h_errno == NO_RECOVERY);
    int pair[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, pair);
    char buf[16] = {0};
    send(pair[0], "ping", 4, 0);
    recv(pair[1], buf, sizeof buf, 0);
    int server = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un sun = {.sun_family = AF_UNIX};
    strcpy(sun.sun_path, "/tmp/socket");
    printf("socketpair %s bind %d listen %d", buf, bind(server, (struct sockaddr*)&sun, sizeof sun), listen(server, 4));
    struct stat st;
    stat("/tmp/socket", &st);
    int client = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    printf(" %d connect %d", S_ISSOCK(st.st_mode), connect(client, (struct sockaddr*)&sun, sizeof sun));
    struct sockaddr_un name;
    socklen_t length = sizeof name;
    int conn = accept(server, 0, 0);
    getsockname(server, (struct sockaddr*)&name, &length);
    printf(" accept %d getsockname %s", conn >= 0, name.sun_path);
    char data = 'x';
    struct iovec iov = {&data, 1};
    union {
        struct cmsghdr header;
        char space[CMSG_SPACE(sizeof(int))];
    } control;
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = &control, .msg_controllen = sizeof control};
    struct cmsghdr* c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    int sent = pair[0];
    memcpy(CMSG_DATA(c), &sent, sizeof sent);
    printf(" sendmsg %zd", sendmsg(client, &msg, 0));
    memset(&control, 0, sizeof control);
    msg.msg_controllen = sizeof control;
    data = 0;
    ssize_t got = recvmsg(conn, &msg, 0);
    int passed;
    memcpy(&passed, CMSG_DATA(CMSG_FIRSTHDR(&msg)), sizeof passed);
    write(passed, "pass", 4);
    memset(buf, 0, sizeof buf);
    recv(pair[1], buf, sizeof buf, 0);
    printf(" recvmsg %zd %c %s", got, data, buf);
    struct timeval tv = {0, 50000};
    setsockopt(conn, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    errno = 0;
    printf(" timeout %zd %d\n", recv(conn, buf, 1, 0), errno == EAGAIN);
    struct if_nameindex* ifs = if_nameindex();
    printf("if_nameindex %d crypt %s", ifs && ifs[0].if_index > 0, crypt("secret", "$6$salt"));
    printf(" %s\n", crypt("secret", "xy"));
    if_freenameindex(ifs);
}

static void rest(void) {
    fenv_t env;
    fexcept_t flags;
    printf("fenv %d %d %d %d %d", fegetexceptflag(&flags, FE_ALL_EXCEPT), feholdexcept(&env), fesetround(FE_TONEAREST),
           feupdateenv(&env), fesetexceptflag(&flags, FE_ALL_EXCEPT));
    char host[64];
    printf(" gethostname %d", gethostname(host, sizeof host) == 0 && host[0]);
    printf(" confstr %zu sysconf %d pathconf %ld\n", confstr(_CS_PATH, 0, 0), sysconf(_SC_PAGESIZE) == 4096,
           pathconf("/", _PC_NAME_MAX));
}

int main(void) {
    setvbuf(stdout, 0, _IOLBF, 0);
    strings();
    wide();
    classes();
    locales();
    times();
    environment();
    randoms();
    heap();
    files();
    users();
    signals();
    network();
    rest();

    return 0;
}
