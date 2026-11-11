//@ host: false
//@ compile-flags: -g
//@ stdout: "process identity: ok\n"

#include <errno.h>
#include <grp.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void check(bool condition) {
    if (!condition) {
        abort();
    }
}

int main() {
    uid_t real, effective, saved;
    gid_t groupReal, groupEffective, groupSaved;
    check(getresuid(&real, &effective, &saved) == 0);
    check(getresgid(&groupReal, &groupEffective, &groupSaved) == 0);
    check(real == getuid() && effective == geteuid());
    check(groupReal == getgid() && groupEffective == getegid());
    check(setuid(real) == 0 && seteuid(effective) == 0);
    check(setgid(groupReal) == 0 && setegid(groupEffective) == 0);
    check(setreuid(uid_t(-1), effective) == 0);
    check(setregid(gid_t(-1), groupEffective) == 0);
    check(setresuid(real, effective, saved) == 0);
    check(setresgid(groupReal, groupEffective, groupSaved) == 0);
    check(setuid(uid_t(-2)) == -1 && errno == EPERM);
    check(seteuid(uid_t(-2)) == -1 && errno == EPERM);
    check(setgid(gid_t(-2)) == -1 && errno == EPERM);
    check(setegid(gid_t(-2)) == -1 && errno == EPERM);
    check(setreuid(uid_t(-2), uid_t(-1)) == -1 && errno == EPERM);
    check(setregid(gid_t(-2), gid_t(-1)) == -1 && errno == EPERM);
    check(setresuid(real, effective, uid_t(-2)) == -1 && errno == EPERM);
    check(setresgid(groupReal, groupEffective, gid_t(-2)) == -1 && errno == EPERM);
    int count = getgroups(0, nullptr);
    check(count >= 0);
    auto groups = static_cast<gid_t*>(malloc((size_t(count) + 1) * sizeof(gid_t)));
    check(groups != nullptr && getgroups(count + 1, groups) == count);
    if (count > 1) {
        check(getgroups(count - 1, groups) == -1 && errno == EINVAL);
    }
    check(setgroups(0, nullptr) == -1 && errno == EPERM);
    check(initgroups("nobody", groupReal) == -1 && errno == EPERM);
    free(groups);

    check(getpid() > 0 && getsid(0) > 0 && getpgid(0) == getpgrp());
    check(getsid(0x7fffffff) == -1 && errno == ESRCH);
    check(getpgid(0x7fffffff) == -1 && errno == ESRCH);
    check(setpgid(0x7fffffff, 0) == -1 && errno == ESRCH);
    check(setpgid(0, 0x7fffffff) == -1 && errno == EPERM);
    check(setsid() == -1 && errno == EPERM);
    int control[2];
    check(pipe(control) == 0);
    pid_t child = fork();
    check(child >= 0);
    if (child == 0) {
        check(getppid() > 0 && setsid() == getpid());
        check(getsid(0) == getpid() && getpgrp() == getpid());
        char byte;
        check(read(control[0], &byte, 1) == 1 && byte == 'r');
        check(getpriority(PRIO_PROCESS, 0) == 19);
        _exit(7);
    }
    check(getpriority(PRIO_PROCESS, child) >= PRIO_MIN);
    check(setpriority(PRIO_PROCESS, child, 19) == 0);
    check(getpriority(PRIO_PROCESS, child) == 19);
    check(write(control[1], "r", 1) == 1);
    int status = 0;
    check(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 7);
    check(close(control[0]) == 0 && close(control[1]) == 0);
    check(setpgrp() == 0);

    int priority = getpriority(PRIO_PROCESS, 0);
    check(priority >= PRIO_MIN && priority <= 19);
    check(setpriority(PRIO_PROCESS, 0, 19) == 0);
    check(getpriority(PRIO_PROCESS, getpid()) == 19);
    check(getpriority(PRIO_PGRP, getpgrp()) == 19);
    check(getpriority(PRIO_USER, real) == 19);
    check(nice(10) == 19);
    check(setpriority(PRIO_PGRP, 0, 19) == 0);
    check(setpriority(PRIO_USER, 0, 19) == 0);
    check(getpriority(99, 0) == -1 && errno == EINVAL);
    check(setpriority(99, 0, 0) == -1 && errno == EINVAL);
    check(getpriority(PRIO_PROCESS, 0x7fffffff) == -1 && errno == ESRCH);
    check(getpriority(PRIO_USER, uid_t(-2)) == -1 && errno == ESRCH);
    check(setpriority(PRIO_USER, uid_t(-2), 19) == -1 && errno == ESRCH);
    if (effective != 0) {
        check(nice(-1) == -1 && errno == EPERM);
        check(setpriority(PRIO_PROCESS, 0, 0) == -1 && errno == EACCES);
    }

    sched_param parameter{};
    check(sched_yield() == 0);
    check(sched_get_priority_min(SCHED_FIFO) == 1 && sched_get_priority_max(SCHED_FIFO) == 99);
    check(sched_get_priority_min(SCHED_RR) == 1 && sched_get_priority_max(SCHED_RR) == 99);
    const int policies[] = {SCHED_OTHER, SCHED_BATCH, SCHED_IDLE};
    for (int policy : policies) {
        check(sched_get_priority_min(policy) == 0 && sched_get_priority_max(policy) == 0);
        check(sched_setscheduler(0, policy, &parameter) == 0);
        check(sched_getscheduler(0) == policy);
    }
    check(sched_getparam(0, &parameter) == 0 && parameter.sched_priority == 0);
    check(sched_setparam(0, &parameter) == 0);
    parameter.sched_priority = 1;
    check(sched_setparam(0, &parameter) == -1 && errno == EINVAL);
    check(sched_setscheduler(0, SCHED_OTHER, &parameter) == -1 && errno == EINVAL);
    check(sched_setscheduler(0, SCHED_FIFO, &parameter) == -1 && errno == EPERM);
    check(sched_setscheduler(0, 99, &parameter) == -1 && errno == EINVAL);
    check(sched_get_priority_min(99) == -1 && errno == EINVAL);
    check(sched_get_priority_max(99) == -1 && errno == EINVAL);
    check(sched_getscheduler(0x7fffffff) == -1 && errno == ESRCH);
    check(sched_getparam(0x7fffffff, &parameter) == -1 && errno == ESRCH);
    check(sched_setparam(0x7fffffff, &parameter) == -1 && errno == ESRCH);
    check(sched_setscheduler(0x7fffffff, SCHED_OTHER, &parameter) == -1 && errno == ESRCH);
    timespec interval{};
    check(sched_rr_get_interval(0, &interval) == 0 && (interval.tv_sec > 0 || interval.tv_nsec > 0));
    check(sched_rr_get_interval(0x7fffffff, &interval) == -1 && errno == ESRCH);

    char name[16]{};
    check(prctl(PR_SET_NAME, "identity-case") == 0);
    check(prctl(PR_GET_NAME, name) == 0 && strcmp(name, "identity-case") == 0);
    check(prctl(PR_SET_DUMPABLE, 0) == 0 && prctl(PR_GET_DUMPABLE) == 0);
    check(prctl(PR_SET_DUMPABLE, 1) == 0 && prctl(PR_GET_DUMPABLE) == 1);
    check(prctl(PR_SET_DUMPABLE, 2) == -1 && errno == EINVAL);
    check(prctl(PR_SET_NO_NEW_PRIVS, 1) == 0 && prctl(PR_GET_NO_NEW_PRIVS) == 1);
    check(prctl(PR_SET_NO_NEW_PRIVS, 0) == -1 && errno == EINVAL);
    int deathSignal = 0;
    check(prctl(PR_SET_PDEATHSIG, SIGTERM) == 0);
    check(prctl(PR_GET_PDEATHSIG, &deathSignal) == 0 && deathSignal == SIGTERM);
    check(prctl(PR_SET_PDEATHSIG, 1000) == -1 && errno == EINVAL);
    check(prctl(PR_SET_PDEATHSIG, 0) == 0);
    check(prctl(-1) == -1 && errno == EINVAL);

    rlimit limit{}, observed{};
    check(getrlimit(RLIMIT_NOFILE, &limit) == 0);
    check(prlimit(0, RLIMIT_NOFILE, nullptr, &observed) == 0 && observed.rlim_cur == limit.rlim_cur);
    observed.rlim_cur = observed.rlim_max + 1;
    check(setrlimit(RLIMIT_NOFILE, &observed) == -1 && errno == EINVAL);
    observed = limit;
    observed.rlim_cur = 3;
    check(prlimit(0, RLIMIT_NOFILE, &observed, nullptr) == 0 && getdtablesize() == 3);
    check(setrlimit(RLIMIT_NOFILE, &limit) == 0);
    if (effective != 0 && limit.rlim_max != RLIM_INFINITY) {
        observed = limit;
        observed.rlim_max++;
        check(setrlimit(RLIMIT_NOFILE, &observed) == -1 && errno == EPERM);
    }
    check(getrlimit(99, &observed) == -1 && errno == EINVAL);
    check(prlimit(0x7fffffff, RLIMIT_NOFILE, nullptr, &observed) == -1 && errno == ESRCH);
    rusage usage{};
    check(getrusage(RUSAGE_SELF, &usage) == 0);
    check(getrusage(RUSAGE_CHILDREN, &usage) == 0);
    check(getrusage(99, &usage) == -1 && errno == EINVAL);
    puts("process identity: ok");
    return 0;
}
