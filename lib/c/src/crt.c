#include "kernel.h"

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

int errno;
char** environ;
char* program_invocation_name;
char* program_invocation_short_name;
int h_errno;
char* tzname[2] = {"UTC", "UTC"};
long timezone;
int daylight;
const struct in6_addr in6addr_any = IN6ADDR_ANY_INIT;
const struct in6_addr in6addr_loopback = IN6ADDR_LOOPBACK_INIT;

int __main_argc_argv(int, char**);

static char* lastComponent(char* path) {
    char* last = path;

    for (char* p = path; *p; p++) {
        if (*p == '/') {
            last = p + 1;
        }
    }

    return last;
}

__attribute__((__export_name__("_start"))) void _start(void) {
    int argc = 0;
    char** argv = 0;

    __kernel_start(&errno, &environ, &argc, &argv, tzname, &timezone, &daylight);

    if (argc > 0) {
        program_invocation_name = argv[0];
        program_invocation_short_name = lastComponent(argv[0]);
    }

    exit(__main_argc_argv(argc, argv));
}

__attribute__((__export_name__("__call_signal_handler"))) void __call_signal_handler(void (*handler)(int), int signal) {
    handler(signal);
}

__attribute__((__export_name__("__call_signal_action"))) void __call_signal_action(void (*action)(int, siginfo_t*, void*), int signal, siginfo_t* info, void* context) {
    action(signal, info, context);
}

__attribute__((__export_name__("__call_glob_error"))) int __call_glob_error(int (*error)(const char*, int), const char* path, int code) {
    return error(path, code);
}
