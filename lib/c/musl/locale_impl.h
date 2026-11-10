#pragma once

#include <features.h>
#include <locale.h>
#include <stdlib.h>

/* The wide stdio functions make a stream's locale current for the call:
   lib/c has the one locale, so nothing reads it (src/locale.c). */
extern locale_t __current_locale;
#define CURRENT_LOCALE __current_locale
#define CURRENT_UTF8 1
#define C_LOCALE ((locale_t)0)
#define UTF8_LOCALE ((locale_t)0)
#define LCTRANS(msg, lc, loc) (msg)
#define LCTRANS_CUR(msg) (msg)

#undef MB_CUR_MAX
#define MB_CUR_MAX 4
#define __lctrans_cur(msg) (msg)
