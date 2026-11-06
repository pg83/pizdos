#pragma once

#define CHAR_BIT 8
#define SCHAR_MIN (-128)
#define SCHAR_MAX 127
#define UCHAR_MAX 255

#ifdef __CHAR_UNSIGNED__
#define CHAR_MIN 0
#define CHAR_MAX 255
#else
#define CHAR_MIN (-128)
#define CHAR_MAX 127
#endif

#define SHRT_MIN (-1 - SHRT_MAX)
#define SHRT_MAX __SHRT_MAX__
#define USHRT_MAX (SHRT_MAX * 2 + 1)
#define INT_MIN (-1 - INT_MAX)
#define INT_MAX __INT_MAX__
#define UINT_MAX (INT_MAX * 2U + 1U)
#define LONG_MIN (-1L - LONG_MAX)
#define LONG_MAX __LONG_MAX__
#define ULONG_MAX (LONG_MAX * 2UL + 1UL)
#define LLONG_MIN (-1LL - LLONG_MAX)
#define LLONG_MAX __LONG_LONG_MAX__
#define ULLONG_MAX (LLONG_MAX * 2ULL + 1ULL)
#define LONG_BIT 32
#define WORD_BIT 32

#define SSIZE_MAX LONG_MAX
#define MB_LEN_MAX 4

#define PATH_MAX 4096
#define NAME_MAX 255
#define PIPE_BUF 4096
#define ARG_MAX 131072
#define LINE_MAX 4096
#define NGROUPS_MAX 32
#define IOV_MAX 1024
#define HOST_NAME_MAX 255
#define LOGIN_NAME_MAX 256
#define TTY_NAME_MAX 32
#define SYMLOOP_MAX 40
#define OPEN_MAX 1024
#define CHILD_MAX 1024
#define NZERO 20
#define PAGESIZE 65536
#define PAGE_SIZE PAGESIZE
#define FILESIZEBITS 64
#define RE_DUP_MAX 255
#define BC_BASE_MAX 99
#define BC_DIM_MAX 2048
#define BC_SCALE_MAX 99
#define BC_STRING_MAX 1000
#define CHARCLASS_NAME_MAX 14
#define COLL_WEIGHTS_MAX 2
#define EXPR_NEST_MAX 32
#define NL_ARGMAX 9
#define NL_LANGMAX 32
#define NL_MSGMAX 32767
#define NL_SETMAX 255
#define NL_TEXTMAX 2048

#define _POSIX_ARG_MAX 4096
#define _POSIX_CHILD_MAX 25
#define _POSIX_LINK_MAX 8
#define _POSIX_MAX_CANON 255
#define _POSIX_MAX_INPUT 255
#define _POSIX_NAME_MAX 14
#define _POSIX_NGROUPS_MAX 8
#define _POSIX_OPEN_MAX 20
#define _POSIX_PATH_MAX 256
#define _POSIX_PIPE_BUF 512
#define _POSIX_SSIZE_MAX 32767
#define _POSIX_STREAM_MAX 8
#define _POSIX_TZNAME_MAX 6
#define _POSIX2_LINE_MAX 2048
#define _POSIX2_RE_DUP_MAX 255
