#pragma once

#include <features.h>
#include_next <stdlib.h>

hidden void __qsort_r(void*, size_t, size_t, int (*)(const void*, const void*, void*), void*);
