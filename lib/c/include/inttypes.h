#pragma once

#include <stdint.h>

__BEGIN_DECLS

typedef struct {
    intmax_t quot;
    intmax_t rem;
} imaxdiv_t;

intmax_t imaxabs(intmax_t);
imaxdiv_t imaxdiv(intmax_t, intmax_t);
intmax_t strtoimax(const char* __restrict, char** __restrict, int);
uintmax_t strtoumax(const char* __restrict, char** __restrict, int);

__END_DECLS

#define __PRI64 "ll"
#define __PRIPTR "l"

#define PRId8 "d"
#define PRId16 "d"
#define PRId32 "d"
#define PRId64 __PRI64 "d"
#define PRIdLEAST8 "d"
#define PRIdLEAST16 "d"
#define PRIdLEAST32 "d"
#define PRIdLEAST64 __PRI64 "d"
#define PRIdFAST8 "d"
#define PRIdFAST16 "d"
#define PRIdFAST32 "d"
#define PRIdFAST64 __PRI64 "d"
#define PRIdMAX __PRI64 "d"
#define PRIdPTR __PRIPTR "d"

#define PRIi8 "i"
#define PRIi16 "i"
#define PRIi32 "i"
#define PRIi64 __PRI64 "i"
#define PRIiMAX __PRI64 "i"
#define PRIiPTR __PRIPTR "i"

#define PRIo8 "o"
#define PRIo16 "o"
#define PRIo32 "o"
#define PRIo64 __PRI64 "o"
#define PRIoMAX __PRI64 "o"
#define PRIoPTR __PRIPTR "o"

#define PRIu8 "u"
#define PRIu16 "u"
#define PRIu32 "u"
#define PRIu64 __PRI64 "u"
#define PRIuLEAST8 "u"
#define PRIuLEAST16 "u"
#define PRIuLEAST32 "u"
#define PRIuLEAST64 __PRI64 "u"
#define PRIuFAST8 "u"
#define PRIuFAST16 "u"
#define PRIuFAST32 "u"
#define PRIuFAST64 __PRI64 "u"
#define PRIuMAX __PRI64 "u"
#define PRIuPTR __PRIPTR "u"

#define PRIx8 "x"
#define PRIx16 "x"
#define PRIx32 "x"
#define PRIx64 __PRI64 "x"
#define PRIxMAX __PRI64 "x"
#define PRIxPTR __PRIPTR "x"

#define PRIX8 "X"
#define PRIX16 "X"
#define PRIX32 "X"
#define PRIX64 __PRI64 "X"
#define PRIXMAX __PRI64 "X"
#define PRIXPTR __PRIPTR "X"

#define SCNd8 "hhd"
#define SCNd16 "hd"
#define SCNd32 "d"
#define SCNd64 __PRI64 "d"
#define SCNdMAX __PRI64 "d"
#define SCNdPTR __PRIPTR "d"
#define SCNi8 "hhi"
#define SCNi16 "hi"
#define SCNi32 "i"
#define SCNi64 __PRI64 "i"
#define SCNu8 "hhu"
#define SCNu16 "hu"
#define SCNu32 "u"
#define SCNu64 __PRI64 "u"
#define SCNuMAX __PRI64 "u"
#define SCNuPTR __PRIPTR "u"
#define SCNx8 "hhx"
#define SCNx16 "hx"
#define SCNx32 "x"
#define SCNx64 __PRI64 "x"
#define SCNxMAX __PRI64 "x"
#define SCNxPTR __PRIPTR "x"
#define SCNo8 "hho"
#define SCNo16 "ho"
#define SCNo32 "o"
#define SCNo64 __PRI64 "o"
