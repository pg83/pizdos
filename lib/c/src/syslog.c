#include "kernel.h"

#include <stdarg.h>
#include <stdio.h>
#include <syslog.h>

void vsyslog(int priority, const char* format, va_list args) {
    char message[1024];

    vsnprintf(message, sizeof message, format, args);
    __kernel_syslog(priority, message);
}

void syslog(int priority, const char* format, ...) {
    va_list args;

    va_start(args, format);
    vsyslog(priority, format, args);
    va_end(args);
}
