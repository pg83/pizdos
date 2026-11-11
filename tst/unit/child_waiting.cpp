//@ compile-flags: -g -D_GNU_SOURCE
//@ stdout: "child waiting: ok\n"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <regex.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t alarms;

static void alarmHandler(int signal) {
    assert(signal == SIGALRM);
    alarms = alarms + 1;
}

int main() {
    int status;
    siginfo_t info{};
    assert(waitpid(-1, &status, 0x80) == -1 && errno == EINVAL);
    assert(waitid(static_cast<idtype_t>(999), 0, &info, WEXITED) == -1 && errno == EINVAL);
    assert(waitid(P_PID, 0, &info, WEXITED) == -1 && errno == EINVAL);
    assert(waitid(P_ALL, 0, &info, WNOHANG) == -1 && errno == EINVAL);
    assert(waitid(P_ALL, 0, &info, WEXITED | 0x80) == -1 && errno == EINVAL);
    assert(waitid(P_ALL, 0, &info, WEXITED) == -1 && errno == ECHILD);
    regex_t expression{};
    assert(regcomp(&expression, "^ab+c$", REG_EXTENDED) == 0);
    int fd = open("wait.data", O_RDWR | O_CREAT | O_TRUNC, 0600);
    assert(fd >= 0 && ftruncate(fd, 4096) == 0);
    auto* shared = static_cast<char*>(mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    assert(shared != MAP_FAILED);
    int release[2], ready[2];
    assert(pipe(release) == 0 && pipe(ready) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        assert(regexec(&expression, "abbbc", 0, nullptr, 0) == 0);
        regfree(&expression);
        assert(setpgid(0, 0) == 0);
        assert(write(ready[1], "r", 1) == 1);
        assert(raise(SIGSTOP) == 0);
        char byte;
        assert(read(release[0], &byte, 1) == 1 && byte == 'x');
        shared[0] = 's';
        assert(msync(shared, 4096, MS_SYNC) == 0);
        _exit(17);
    }
    char byte;
    assert(read(ready[0], &byte, 1) == 1 && byte == 'r');
    assert(waitid(P_PGID, child, &info, WSTOPPED | WNOWAIT) == 0);
    assert(info.si_pid == child && info.si_signo == SIGCHLD && info.si_code == CLD_STOPPED && info.si_status == SIGSTOP);
    // Linux's clone selector excludes ordinary fork children; __WALL includes them.
    assert(waitpid(child, &status, int(0x80000000U) | WNOHANG) == -1 && errno == ECHILD);
    assert(waitpid(-child, &status, WUNTRACED | 0x40000000) == child && WIFSTOPPED(status) && WSTOPSIG(status) == SIGSTOP);
    assert(kill(child, SIGCONT) == 0);
    assert(waitid(P_PID, child, &info, WCONTINUED | WNOWAIT) == 0);
    assert(info.si_pid == child && info.si_code == CLD_CONTINUED && info.si_status == SIGCONT);
    assert(waitpid(-child, &status, WCONTINUED) == child && WIFCONTINUED(status));
    assert(write(release[1], "x", 1) == 1);
    assert(waitid(P_PID, child, &info, WEXITED | WNOWAIT) == 0);
    assert(info.si_pid == child && info.si_code == CLD_EXITED && info.si_status == 17);
    rusage usage{};
    assert(wait3(&status, 0, &usage) == child && WIFEXITED(status) && WEXITSTATUS(status) == 17);
    assert(usage.ru_utime.tv_sec >= 0 && usage.ru_stime.tv_sec >= 0);
    assert(pread(fd, &byte, 1, 0) == 1 && byte == 's');
    assert(regexec(&expression, "abbbc", 0, nullptr, 0) == 0);
    regfree(&expression);
    assert(munmap(shared, 4096) == 0 && close(fd) == 0 && unlink("wait.data") == 0);

    child = fork();
    assert(child >= 0);
    if (child == 0) {
        raise(SIGTERM);
        _exit(1);
    }
    assert(waitid(P_ALL, 0, &info, WEXITED | WNOWAIT) == 0);
    assert(info.si_pid == child && info.si_code == CLD_KILLED && info.si_status == SIGTERM);
    assert(waitpid(0, &status, 0) == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);

    child = fork();
    assert(child >= 0);
    if (child == 0) {
        assert(write(ready[1], "r", 1) == 1);
        assert(read(release[0], &byte, 1) == 1);
        _exit(23);
    }
    assert(read(ready[0], &byte, 1) == 1 && byte == 'r');
    memset(&info, 0xa5, sizeof(info));
    assert(waitid(P_PGID, 0, &info, WEXITED | WNOHANG) == 0 && info.si_pid == 0);
    struct sigaction action{};
    action.sa_handler = alarmHandler;
    assert(sigemptyset(&action.sa_mask) == 0 && sigaction(SIGALRM, &action, nullptr) == 0);
    ualarm(40000, 0);
    assert(waitpid(child, &status, 0) == -1 && errno == EINTR && alarms == 1);
    assert(write(release[1], "x", 1) == 1);
    assert(wait4(child, &status, 0, &usage) == child && WIFEXITED(status) && WEXITSTATUS(status) == 23);
    assert(waitpid(child, &status, WNOHANG) == -1 && errno == ECHILD);
    assert(close(release[0]) == 0 && close(release[1]) == 0 && close(ready[0]) == 0 && close(ready[1]) == 0);
    puts("child waiting: ok");
}
