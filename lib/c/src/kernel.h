#pragma once

#include <__base.h>

/*
 * Imports private to lib/c. __kernel_start hands the kernel the addresses it
 * keeps current for the process (errno, environ, and tzname, timezone and
 * daylight, which tzset sets) and receives the argument vector, which the
 * kernel has already placed in the process heap.
 */
void __kernel_start(int* errnoAt, char*** environAt, int* argc, char*** argv, char** tznameAt, long* timezoneAt,
                    int* daylightAt) __KERNEL(__kernel_start);
void __kernel_syslog(int priority, const char* message) __KERNEL(__kernel_syslog);
