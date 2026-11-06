#pragma once

#include <__base.h>

__BEGIN_DECLS

struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
    long tm_gmtoff;
    const char* tm_zone;
};

struct itimerspec {
    struct timespec it_interval;
    struct timespec it_value;
};

#define CLOCKS_PER_SEC 1000000L
#define TIME_UTC 1

#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_THREAD_CPUTIME_ID 3
#define CLOCK_MONOTONIC_RAW 4
#define CLOCK_REALTIME_COARSE 5
#define CLOCK_MONOTONIC_COARSE 6
#define CLOCK_BOOTTIME 7
#define TIMER_ABSTIME 1

extern char* tzname[2];
extern long timezone;
extern int daylight;

time_t time(time_t*) __KERNEL(time);
clock_t clock(void) __KERNEL(clock);
double difftime(time_t, time_t) __KERNEL(difftime);
time_t mktime(struct tm*) __KERNEL(mktime);
time_t timegm(struct tm*) __KERNEL(timegm);
struct tm* gmtime(const time_t*) __KERNEL(gmtime);
struct tm* gmtime_r(const time_t* __restrict, struct tm* __restrict) __KERNEL(gmtime_r);
struct tm* localtime(const time_t*) __KERNEL(localtime);
struct tm* localtime_r(const time_t* __restrict, struct tm* __restrict) __KERNEL(localtime_r);
char* asctime(const struct tm*) __KERNEL(asctime);
char* asctime_r(const struct tm* __restrict, char* __restrict) __KERNEL(asctime_r);
char* ctime(const time_t*) __KERNEL(ctime);
char* ctime_r(const time_t*, char*) __KERNEL(ctime_r);
size_t strftime(char* __restrict, size_t, const char* __restrict, const struct tm* __restrict) __KERNEL(strftime);
char* strptime(const char* __restrict, const char* __restrict, struct tm* __restrict) __KERNEL(strptime);
void tzset(void) __KERNEL(tzset);
int nanosleep(const struct timespec*, struct timespec*) __KERNEL(nanosleep);
int clock_gettime(clockid_t, struct timespec*) __KERNEL(clock_gettime);
int clock_settime(clockid_t, const struct timespec*) __KERNEL(clock_settime);
int clock_getres(clockid_t, struct timespec*) __KERNEL(clock_getres);
int clock_nanosleep(clockid_t, int, const struct timespec*, struct timespec*) __KERNEL(clock_nanosleep);
int stime(const time_t*) __KERNEL(stime);

__END_DECLS
