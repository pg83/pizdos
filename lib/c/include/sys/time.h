#pragma once

#include <__base.h>
#include <sys/select.h>

__BEGIN_DECLS

struct timezone {
    int tz_minuteswest;
    int tz_dsttime;
};

struct itimerval {
    struct timeval it_interval;
    struct timeval it_value;
};

#define ITIMER_REAL 0
#define ITIMER_VIRTUAL 1
#define ITIMER_PROF 2

#define timerisset(t) ((t)->tv_sec || (t)->tv_usec)
#define timerclear(t) ((t)->tv_sec = (t)->tv_usec = 0)
#define timercmp(s, t, op) ((s)->tv_sec == (t)->tv_sec ? (s)->tv_usec op(t)->tv_usec : (s)->tv_sec op(t)->tv_sec)
#define timeradd(s, t, a) (void)((a)->tv_sec = (s)->tv_sec + (t)->tv_sec, ((a)->tv_usec = (s)->tv_usec + (t)->tv_usec) >= 1000000 && ((a)->tv_usec -= 1000000, (a)->tv_sec++))
#define timersub(s, t, a) (void)((a)->tv_sec = (s)->tv_sec - (t)->tv_sec, ((a)->tv_usec = (s)->tv_usec - (t)->tv_usec) < 0 && ((a)->tv_usec += 1000000, (a)->tv_sec--))

int gettimeofday(struct timeval* __restrict, void* __restrict) __KERNEL(gettimeofday);
int settimeofday(const struct timeval*, const struct timezone*) __KERNEL(settimeofday);
int getitimer(int, struct itimerval*) __KERNEL(getitimer);
int setitimer(int, const struct itimerval* __restrict, struct itimerval* __restrict) __KERNEL(setitimer);
int utimes(const char*, const struct timeval[2]) __KERNEL(utimes);
int lutimes(const char*, const struct timeval[2]) __KERNEL(lutimes);
int futimes(int, const struct timeval[2]) __KERNEL(futimes);
int adjtime(const struct timeval*, struct timeval*) __KERNEL(adjtime);

__END_DECLS
