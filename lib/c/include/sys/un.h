#pragma once

#include <__base.h>

struct sockaddr_un {
    sa_family_t sun_family;
    char sun_path[108];
};

#define SUN_LEN(s) (2 + __builtin_strlen((s)->sun_path))
