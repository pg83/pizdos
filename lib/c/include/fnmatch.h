#pragma once

#include <__base.h>

__BEGIN_DECLS

#define FNM_PATHNAME 0x1
#define FNM_NOESCAPE 0x2
#define FNM_PERIOD 0x4
#define FNM_LEADING_DIR 0x8
#define FNM_CASEFOLD 0x10
#define FNM_FILE_NAME FNM_PATHNAME
#define FNM_NOMATCH 1
#define FNM_NOSYS (-1)

int fnmatch(const char*, const char*, int);

__END_DECLS
