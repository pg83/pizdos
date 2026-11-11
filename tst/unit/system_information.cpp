//@ host: false
//@ compile-flags: -g
//@ stdout: "system information: ok\n"
//@ stderr: "system: visible 7\nsystem: newline\nsystem[1]: pid\n"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysinfo.h>
#include <sys/times.h>
#include <sys/utsname.h>
#include <syslog.h>
#include <unistd.h>

static void checkAt(bool condition, int line) {
    if (!condition) {
        fprintf(stderr, "check failed at line %d (errno %d)\n", line, errno);
        abort();
    }
}
#define check(condition) checkAt((condition), __LINE__)

int main() {
    struct sysinfo information{};
    check(sysinfo(&information) == 0);
    check(information.totalram > 0 && information.mem_unit > 0 && information.procs >= 1);
    check(information.freeram <= information.totalram);
    check(get_nprocs() > 0 && get_nprocs_conf() > 0);
    check(get_phys_pages() > 0 && get_avphys_pages() >= 0);
    double loads[] = {-1, -1, -1, 42};
    check(getloadavg(nullptr, 0) == 0 && getloadavg(loads, -1) == -1);
    check(getloadavg(loads, 4) == 3 && loads[3] == 42);
    check(loads[0] >= 0 && loads[1] >= 0 && loads[2] >= 0);
    const int queries[] = {_SC_ARG_MAX, _SC_CHILD_MAX, _SC_CLK_TCK, _SC_NGROUPS_MAX, _SC_OPEN_MAX, _SC_STREAM_MAX, _SC_TZNAME_MAX, _SC_JOB_CONTROL, _SC_SAVED_IDS, _SC_VERSION, _SC_PAGESIZE, _SC_LINE_MAX, _SC_IOV_MAX, _SC_GETGR_R_SIZE_MAX, _SC_GETPW_R_SIZE_MAX, _SC_LOGIN_NAME_MAX, _SC_TTY_NAME_MAX, _SC_NPROCESSORS_CONF, _SC_NPROCESSORS_ONLN, _SC_PHYS_PAGES, _SC_AVPHYS_PAGES, _SC_MONOTONIC_CLOCK, _SC_SYMLOOP_MAX, _SC_HOST_NAME_MAX};
    for (int query : queries) {
        errno = 0;
        check(sysconf(query) >= -1 && errno == 0);
    }
    check(sysconf(99999) == -1 && errno == EINVAL);
    check(pathconf("/", _PC_NAME_MAX) > 0);
    check(pathconf("/", 99999) == -1 && errno == EINVAL);
    check(pathconf("/missing", _PC_NAME_MAX) == -1 && errno == ENOENT);
    int directory = open("/", O_RDONLY | O_DIRECTORY);
    check(directory >= 0 && fpathconf(directory, _PC_NAME_MAX) > 0);
    check(fpathconf(directory, 99999) == -1 && errno == EINVAL);
    check(fpathconf(-1, _PC_NAME_MAX) == -1 && errno == EBADF);
    check(close(directory) == 0);
    char path[64];
    size_t needed = confstr(_CS_PATH, nullptr, 0);
    check(needed > 1 && needed < sizeof(path));
    check(confstr(_CS_PATH, path, sizeof(path)) == needed && strlen(path) + 1 == needed);
    check(strchr(path, ':') != nullptr);
    check(confstr(_CS_PATH, path, 1) == needed && path[0] == 0);
    check(confstr(99999, path, sizeof(path)) == 0 && errno == EINVAL);
    utsname names{};
    check(uname(&names) == 0 && names.sysname[0] && names.machine[0]);
    char host[256], domain[256];
    check(gethostname(host, sizeof(host)) == 0 && host[0]);
    check(gethostname(host, 1) == 0 && host[0] == 0);
    check(gethostname(nullptr, 0) == 0);
    check(getdomainname(domain, sizeof(domain)) == 0);
    check(getdomainname(nullptr, 0) == -1 && errno == EINVAL);
    check(gethostid() == 0);
    check(sethostname("changed", 7) == -1 && errno == EPERM);
    check(setdomainname("changed", 7) == -1 && errno == EPERM);
    check(sethostid(1) == -1 && errno == EPERM);
    check(vhangup() == -1 && errno == EPERM);
    tms timesUsed{};
    check(times(&timesUsed) != clock_t(-1));

    check(unsetenv("LOGNAME") == 0 && getlogin() == nullptr);
    char login[32];
    check(getlogin_r(login, sizeof(login)) == ENXIO);
    check(setenv("LOGNAME", "coverage-user", 1) == 0);
    check(strcmp(getlogin(), "coverage-user") == 0);
    check(getlogin_r(login, sizeof(login)) == 0 && strcmp(login, "coverage-user") == 0);
    check(getlogin_r(login, 2) == ERANGE);
    char terminal[L_ctermid];
    check(ctermid(terminal) == terminal && terminal[0] == '/');
    check(strcmp(ctermid(nullptr), terminal) == 0);
    check(ttyname(-1) == nullptr && errno == EBADF);
    check(ttyname_r(-1, terminal, sizeof(terminal)) == EBADF);
    check(ttyname(1) == nullptr && errno == ENOTTY);
    check(ttyname_r(1, terminal, sizeof(terminal)) == ENOTTY);

    openlog("system", LOG_PERROR, LOG_USER);
    int previous = setlogmask(LOG_UPTO(LOG_WARNING));
    check(setlogmask(0) == LOG_UPTO(LOG_WARNING));
    syslog(LOG_INFO, "hidden");
    syslog(LOG_ERR, "visible %d", 7);
    syslog(LOG_WARNING, "newline\n");
    openlog("system", LOG_PERROR | LOG_PID, LOG_USER);
    syslog(LOG_ERR, "pid");
    openlog(nullptr, 0, LOG_USER);
    syslog(LOG_ERR, "no stderr without LOG_PERROR");
    check(setlogmask(previous) == LOG_UPTO(LOG_WARNING));
    closelog();
    puts("system information: ok");
    return 0;
}
