#pragma once

#include <__base.h>

__BEGIN_DECLS

struct option {
    const char* name;
    int has_arg;
    int* flag;
    int val;
};

#define no_argument 0
#define required_argument 1
#define optional_argument 2

extern char* optarg;
extern int optind;
extern int opterr;
extern int optopt;
extern int optreset;

int getopt(int, char* const[], const char*);
int getopt_long(int, char* const*, const char*, const struct option*, int*);
int getopt_long_only(int, char* const*, const char*, const struct option*, int*);

__END_DECLS
