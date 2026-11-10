#include <ctype.h>
#include <errno.h>
#include <langinfo.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>
#include <wctype.h>

/*
 * The locale_t interface over the one locale lib/c has, C with UTF-8
 * multibyte strings: newlocale takes the names setlocale takes and gives
 * that locale, and every _l function is the function without the locale.
 */
struct __locale_struct {
    int unused;
};

static struct __locale_struct cLocale;
static locale_t current = LC_GLOBAL_LOCALE;

// what musl's wide stdio functions save and restore (musl/locale_impl.h)
locale_t __current_locale;

// utf8 is whether a locale name's codeset is UTF-8, as in en_US.UTF-8.
static int utf8(const char* name) {
    const char* dot = strchr(name, '.');

    if (!dot) {
        return 0;
    }

    char codeset[8];
    size_t n = 0;

    for (const char* p = dot + 1; *p && *p != '@' && n < sizeof codeset - 1; p++) {
        if (*p != '-') {
            codeset[n++] = (char)tolower((unsigned char)*p);
        }
    }

    codeset[n] = 0;

    return strcmp(codeset, "utf8") == 0;
}

locale_t newlocale(int mask, const char* name, locale_t base) {
    if (!name || (*name && strcmp(name, "C") && strcmp(name, "POSIX") && !utf8(name))) {
        errno = ENOENT;

        return 0;
    }

    return &cLocale;
}

locale_t duplocale(locale_t locale) {
    return &cLocale;
}

void freelocale(locale_t locale) {
}

locale_t uselocale(locale_t locale) {
    locale_t previous = current;

    if (locale) {
        current = locale;
    }

    return previous;
}

int isalnum_l(int c, locale_t locale) {
    return isalnum(c);
}

int isalpha_l(int c, locale_t locale) {
    return isalpha(c);
}

int isblank_l(int c, locale_t locale) {
    return isblank(c);
}

int iscntrl_l(int c, locale_t locale) {
    return iscntrl(c);
}

int isdigit_l(int c, locale_t locale) {
    return isdigit(c);
}

int isgraph_l(int c, locale_t locale) {
    return isgraph(c);
}

int islower_l(int c, locale_t locale) {
    return islower(c);
}

int isprint_l(int c, locale_t locale) {
    return isprint(c);
}

int ispunct_l(int c, locale_t locale) {
    return ispunct(c);
}

int isspace_l(int c, locale_t locale) {
    return isspace(c);
}

int isupper_l(int c, locale_t locale) {
    return isupper(c);
}

int isxdigit_l(int c, locale_t locale) {
    return isxdigit(c);
}

int tolower_l(int c, locale_t locale) {
    return tolower(c);
}

int toupper_l(int c, locale_t locale) {
    return toupper(c);
}

int iswalnum_l(wint_t c, locale_t locale) {
    return iswalnum(c);
}

int iswalpha_l(wint_t c, locale_t locale) {
    return iswalpha(c);
}

int iswblank_l(wint_t c, locale_t locale) {
    return iswblank(c);
}

int iswcntrl_l(wint_t c, locale_t locale) {
    return iswcntrl(c);
}

int iswdigit_l(wint_t c, locale_t locale) {
    return iswdigit(c);
}

int iswgraph_l(wint_t c, locale_t locale) {
    return iswgraph(c);
}

int iswlower_l(wint_t c, locale_t locale) {
    return iswlower(c);
}

int iswprint_l(wint_t c, locale_t locale) {
    return iswprint(c);
}

int iswpunct_l(wint_t c, locale_t locale) {
    return iswpunct(c);
}

int iswspace_l(wint_t c, locale_t locale) {
    return iswspace(c);
}

int iswupper_l(wint_t c, locale_t locale) {
    return iswupper(c);
}

int iswxdigit_l(wint_t c, locale_t locale) {
    return iswxdigit(c);
}

int iswctype_l(wint_t c, wctype_t type, locale_t locale) {
    return iswctype(c, type);
}

wctype_t wctype_l(const char* name, locale_t locale) {
    return wctype(name);
}

wint_t towlower_l(wint_t c, locale_t locale) {
    return towlower(c);
}

wint_t towupper_l(wint_t c, locale_t locale) {
    return towupper(c);
}

int strcoll_l(const char* a, const char* b, locale_t locale) {
    return strcoll(a, b);
}

size_t strxfrm_l(char* restrict d, const char* restrict s, size_t n, locale_t locale) {
    return strxfrm(d, s, n);
}

int wcscoll_l(const wchar_t* a, const wchar_t* b, locale_t locale) {
    return wcscoll(a, b);
}

size_t wcsxfrm_l(wchar_t* restrict d, const wchar_t* restrict s, size_t n, locale_t locale) {
    return wcsxfrm(d, s, n);
}

size_t strftime_l(char* restrict s, size_t n, const char* restrict format, const struct tm* restrict tm, locale_t locale) {
    return strftime(s, n, format, tm);
}

char* nl_langinfo_l(nl_item item, locale_t locale) {
    return nl_langinfo(item);
}
