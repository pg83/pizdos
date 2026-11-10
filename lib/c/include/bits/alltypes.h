#pragma once

/*
 * libc++ built for a musl-like libc (_LIBCPP_HAS_MUSL_LIBC) takes mbstate_t
 * from here, where musl defines it; this libc defines it in wchar.h.
 */
#include <wchar.h>
