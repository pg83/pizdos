#pragma once

#include <stdlib.h>

__BEGIN_DECLS

#define M_TRIM_THRESHOLD (-1)
#define M_TOP_PAD (-2)
#define M_MMAP_THRESHOLD (-3)
#define M_MMAP_MAX (-4)
#define M_ARENA_MAX (-8)

void* memalign(size_t, size_t) __KERNEL(memalign);
size_t malloc_usable_size(void*) __KERNEL(malloc_usable_size);
int mallopt(int, int) __KERNEL(mallopt);
int malloc_trim(size_t) __KERNEL(malloc_trim);

__END_DECLS
