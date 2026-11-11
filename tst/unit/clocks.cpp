//@ host: false
//@ compile-flags: -g
//@ stdout: "clocks: ok\n"

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t alarms;

static void alarmed(int) {
    alarms = alarms + 1;
}

int main() {
    time_t now = time(nullptr), stored = 0;
    assert(now > 0 && time(&stored) >= now && stored >= now);
    timeval wall{};
    assert(gettimeofday(&wall, nullptr) == 0 && wall.tv_sec >= now);
    assert(wall.tv_usec >= 0 && wall.tv_usec < 1000000);
    assert(gettimeofday(nullptr, nullptr) == 0);
    assert(clock() >= 0);
    const clockid_t clocks[] = {CLOCK_REALTIME, CLOCK_MONOTONIC, CLOCK_PROCESS_CPUTIME_ID, CLOCK_THREAD_CPUTIME_ID, CLOCK_MONOTONIC_RAW, CLOCK_REALTIME_COARSE, CLOCK_MONOTONIC_COARSE, CLOCK_BOOTTIME};
    timespec current{}, resolution{};
    for (clockid_t id : clocks) {
        assert(clock_gettime(id, &current) == 0 && current.tv_sec >= 0);
        assert(current.tv_nsec >= 0 && current.tv_nsec < 1000000000);
        assert(clock_getres(id, &resolution) == 0 && resolution.tv_sec >= 0);
        assert(resolution.tv_nsec >= 0 && resolution.tv_nsec < 1000000000);
        assert(clock_getres(id, nullptr) == 0);
    }
    assert(clock_gettime(999, &current) == -1 && errno == EINVAL);
    assert(clock_getres(999, &resolution) == -1 && errno == EINVAL);
    assert(clock_settime(CLOCK_REALTIME, &current) == -1 && errno == EPERM);
    assert(settimeofday(&wall, nullptr) == -1 && errno == EPERM);
    assert(adjtime(&wall, nullptr) == -1 && errno == EPERM);
    assert(stime(&now) == -1 && errno == EPERM);
    timespec zero{}, remaining{};
    assert(nanosleep(&zero, &remaining) == 0 && usleep(0) == 0 && sleep(0) == 0);
    assert(clock_nanosleep(CLOCK_REALTIME, 0, &zero, nullptr) == 0);
    assert(clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &zero, nullptr) == 0);
    assert(clock_nanosleep(CLOCK_BOOTTIME, 0, &zero, nullptr) == 0);
    assert(clock_nanosleep(CLOCK_PROCESS_CPUTIME_ID, 0, &zero, nullptr) == ENOTSUP);
    assert(clock_nanosleep(999, 0, &zero, nullptr) == EINVAL);
    timespec invalid{-1, 0};
    assert(clock_nanosleep(CLOCK_MONOTONIC, 0, &invalid, nullptr) == EINVAL);
    invalid = {0, 1000000000};
    assert(clock_nanosleep(CLOCK_MONOTONIC, 0, &invalid, nullptr) == EINVAL);

    itimerval timer{}, old{};
    assert(getitimer(-1, &old) == -1 && errno == EINVAL);
    assert(setitimer(3, &timer, nullptr) == -1 && errno == EINVAL);
    timer.it_interval.tv_sec = -1;
    assert(setitimer(ITIMER_REAL, &timer, nullptr) == -1 && errno == EINVAL);
    timer = {};
    timer.it_value.tv_usec = 1000000;
    assert(setitimer(ITIMER_REAL, &timer, nullptr) == -1 && errno == EINVAL);
    const int kinds[] = {ITIMER_REAL, ITIMER_VIRTUAL, ITIMER_PROF};
    for (int kind : kinds) {
        timer = {};
        assert(getitimer(kind, &old) == 0 && !timerisset(&old.it_value));
        timer.it_value.tv_sec = 30;
        assert(setitimer(kind, &timer, &old) == 0 && !timerisset(&old.it_value));
        assert(getitimer(kind, &old) == 0 && timerisset(&old.it_value) && old.it_value.tv_sec <= 30);
        timer = {};
        assert(setitimer(kind, &timer, &old) == 0 && timerisset(&old.it_value));
        assert(getitimer(kind, &old) == 0 && !timerisset(&old.it_value));
    }
    assert(alarm(30) == 0);
    unsigned left = alarm(0);
    assert(left > 0 && left <= 30);
    assert(ualarm(30000000, 0) == 0);
    left = ualarm(0, 0);
    assert(left > 0 && left <= 30000000);

    struct sigaction action{};
    action.sa_handler = alarmed;
    sigemptyset(&action.sa_mask);
    assert(sigaction(SIGALRM, &action, nullptr) == 0);
    assert(ualarm(10000, 10000) == 0);
    assert(clock_gettime(CLOCK_MONOTONIC, &current) == 0);
    time_t deadline = current.tv_sec + 5;
    while (alarms < 3) {
        timespec delay{1, 0};
        int result = nanosleep(&delay, &remaining);
        if (result != -1 || errno != EINTR) {
            fprintf(stderr, "nanosleep: result=%d errno=%d alarms=%d\n", result, errno, int(alarms));
        }
        assert(result == -1 && errno == EINTR);
        assert(remaining.tv_sec >= 0 && remaining.tv_sec < 1 && remaining.tv_nsec >= 0);
        assert(clock_gettime(CLOCK_MONOTONIC, &current) == 0 && current.tv_sec <= deadline);
    }
    ualarm(0, 0);
    assert(alarms >= 3);
    invalid = {-1, 0};
    assert(nanosleep(&invalid, nullptr) == -1 && errno == EINVAL);
    invalid = {0, 1000000000};
    assert(nanosleep(&invalid, nullptr) == -1 && errno == EINVAL);
    puts("clocks: ok");
}
