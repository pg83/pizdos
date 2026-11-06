#pragma once

#include <__base.h>

__BEGIN_DECLS

struct sched_param {
    int sched_priority;
};

#define SCHED_OTHER 0
#define SCHED_FIFO 1
#define SCHED_RR 2
#define SCHED_BATCH 3
#define SCHED_IDLE 5

int sched_yield(void) __KERNEL(sched_yield);
int sched_get_priority_max(int) __KERNEL(sched_get_priority_max);
int sched_get_priority_min(int) __KERNEL(sched_get_priority_min);
int sched_getparam(pid_t, struct sched_param*) __KERNEL(sched_getparam);
int sched_setparam(pid_t, const struct sched_param*) __KERNEL(sched_setparam);
int sched_getscheduler(pid_t) __KERNEL(sched_getscheduler);
int sched_setscheduler(pid_t, int, const struct sched_param*) __KERNEL(sched_setscheduler);
int sched_rr_get_interval(pid_t, struct timespec*) __KERNEL(sched_rr_get_interval);

__END_DECLS
