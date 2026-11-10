#pragma once

#include <__base.h>

/*
 * A program is one module, linked whole: there is no object to load and no
 * symbol table to search. dlopen fails and dlsym finds nothing, dlerror
 * says why; dladdr names the program, the one object, for any address.
 */
#define RTLD_LAZY 1
#define RTLD_NOW 2
#define RTLD_NOLOAD 4
#define RTLD_GLOBAL 256
#define RTLD_LOCAL 0
#define RTLD_NODELETE 4096

#define RTLD_NEXT ((void*)-1)
#define RTLD_DEFAULT ((void*)0)

__BEGIN_DECLS

typedef struct {
    const char* dli_fname;
    void* dli_fbase;
    const char* dli_sname;
    void* dli_saddr;
} Dl_info;

void* dlopen(const char*, int);
int dlclose(void*);
void* dlsym(void* __restrict, const char* __restrict);
int dladdr(const void*, Dl_info*);
char* dlerror(void);

__END_DECLS
