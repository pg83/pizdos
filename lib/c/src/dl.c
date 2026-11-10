#include "kernel.h"

#include <dlfcn.h>
#include <limits.h>

static const char* failure;

void* dlopen(const char* file, int flags) {
    failure = "dlopen: a program has no dynamic loading";

    return 0;
}

int dlclose(void* handle) {
    failure = "dlclose: no object was loaded";

    return -1;
}

void* dlsym(void* restrict handle, const char* restrict name) {
    failure = "dlsym: a program has no symbol table";

    return 0;
}

int dladdr(const void* address, Dl_info* info) {
    static char path[PATH_MAX];

    if (!__kernel_program(path, sizeof path)) {
        return 0;
    }

    info->dli_fname = path;
    info->dli_fbase = 0;
    info->dli_sname = 0;
    info->dli_saddr = 0;

    return 1;
}

// dlerror gives the last failure once.
char* dlerror(void) {
    const char* last = failure;
    failure = 0;

    return (char*)last;
}
