#pragma once

#include <stdlib.h>

__BEGIN_DECLS

#define M_TRIM_THRESHOLD (-1)
#define M_TOP_PAD (-2)
#define M_MMAP_THRESHOLD (-3)
#define M_MMAP_MAX (-4)
#define M_ARENA_MAX (-8)

void* memalign(size_t, size_t);
size_t malloc_usable_size(void*);
int mallopt(int, int);
int malloc_trim(size_t);

__END_DECLS
