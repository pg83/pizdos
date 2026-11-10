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

time_t time(time_t*);
clock_t clock(void);
double difftime(time_t, time_t);
time_t mktime(struct tm*);
time_t timegm(struct tm*);
struct tm* gmtime(const time_t*);
struct tm* gmtime_r(const time_t* __restrict, struct tm* __restrict);
struct tm* localtime(const time_t*);
struct tm* localtime_r(const time_t* __restrict, struct tm* __restrict);
char* asctime(const struct tm*);
char* asctime_r(const struct tm* __restrict, char* __restrict);
char* ctime(const time_t*);
char* ctime_r(const time_t*, char*);
size_t strftime(char* __restrict, size_t, const char* __restrict, const struct tm* __restrict);
char* strptime(const char* __restrict, const char* __restrict, struct tm* __restrict);
void tzset(void);
int nanosleep(const struct timespec*, struct timespec*);
int clock_gettime(clockid_t, struct timespec*);
int clock_settime(clockid_t, const struct timespec*);
int clock_getres(clockid_t, struct timespec*);
int clock_nanosleep(clockid_t, int, const struct timespec*, struct timespec*);
int stime(const time_t*);

size_t strftime_l(char* __restrict, size_t, const char* __restrict, const struct tm* __restrict, locale_t);

__END_DECLS
