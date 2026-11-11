//@ compile-flags: -g -D_GNU_SOURCE -fno-builtin
//@ stdout: "character classes: ok\n"

#include <assert.h>
#include <ctype.h>
#include <langinfo.h>
#include <limits.h>
#include <locale.h>
#include <stdio.h>
#include <string.h>
#include <wctype.h>

struct Class {
    const char* name;
    int (*narrow)(int);
    int (*wide)(wint_t);
    int (*narrowLocale)(int, locale_t);
    int (*wideLocale)(wint_t, locale_t);
};

int main() {
    assert(setlocale(LC_ALL, "C.UTF-8"));
    locale_t locale = newlocale(LC_ALL_MASK, "C.UTF-8", nullptr);
    assert(locale);
    const Class classes[] = {
        {"alnum", isalnum, iswalnum, isalnum_l, iswalnum_l},
        {"alpha", isalpha, iswalpha, isalpha_l, iswalpha_l},
        {"blank", isblank, iswblank, isblank_l, iswblank_l},
        {"cntrl", iscntrl, iswcntrl, iscntrl_l, iswcntrl_l},
        {"digit", isdigit, iswdigit, isdigit_l, iswdigit_l},
        {"graph", isgraph, iswgraph, isgraph_l, iswgraph_l},
        {"lower", islower, iswlower, islower_l, iswlower_l},
        {"print", isprint, iswprint, isprint_l, iswprint_l},
        {"punct", ispunct, iswpunct, ispunct_l, iswpunct_l},
        {"space", isspace, iswspace, isspace_l, iswspace_l},
        {"upper", isupper, iswupper, isupper_l, iswupper_l},
        {"xdigit", isxdigit, iswxdigit, isxdigit_l, iswxdigit_l},
    };
    for (int c = -1; c <= 255; ++c) {
        bool alpha = c >= 'A' && c <= 'Z' || c >= 'a' && c <= 'z';
        bool digit = c >= '0' && c <= '9';
        bool graph = c >= '!' && c <= '~';
        bool expected[] = {alpha || digit, alpha, c == ' ' || c == '\t',
                           c >= 0 && c < 32 || c == 127, digit, graph,
                           c >= 'a' && c <= 'z', c >= ' ' && c <= '~', graph && !alpha && !digit,
                           c == ' ' || c >= '\t' && c <= '\r', c >= 'A' && c <= 'Z',
                           digit || c >= 'A' && c <= 'F' || c >= 'a' && c <= 'f'};
        for (unsigned i = 0; i < 12; ++i) {
            const Class& cls = classes[i];
            assert(bool(cls.narrow(c)) == expected[i]);
            assert(bool(cls.narrowLocale(c, locale)) == expected[i]);
            if (c >= 0 && c < 128) {
                assert(bool(cls.wide(c)) == expected[i]);
                assert(bool(cls.wideLocale(c, locale)) == expected[i]);
                assert(bool(iswctype(c, wctype(cls.name))) == expected[i]);
                assert(bool(iswctype_l(c, wctype_l(cls.name, locale), locale)) == expected[i]);
            }
        }
        assert(bool(isascii(c)) == (c >= 0 && c < 128));
        assert(toascii(c) == (c & 127));
        assert(tolower(c) == (c >= 'A' && c <= 'Z' ? c + 32 : c));
        assert(toupper(c) == (c >= 'a' && c <= 'z' ? c - 32 : c));
        assert(tolower_l(c, locale) == tolower(c) && toupper_l(c, locale) == toupper(c));
    }
    struct WideCase {
        wint_t value;
        unsigned mask;
    };
    // Bits follow the public class names above.
    const WideCase wide[] = {
        {0xe9, 1 | 2 | 32 | 64 | 128},
        {0xa0, 32 | 128 | 256},
        {0x2028, 8 | 512},
        {0xfff9, 8},
        {0x1f600, 32 | 128 | 256},
        {0xd800, 0}, {0xffff, 0}, {0x10ffff, 0}, {0x110000, 0}, {static_cast<wint_t>(WEOF), 0},
    };
    for (auto test : wide) {
        for (unsigned i = 0; i < 12; ++i) {
            const Class& cls = classes[i];
            bool expected = (test.mask & (1U << i)) != 0;
            if (bool(cls.wide(test.value)) != expected) {
                fprintf(stderr, "U+%x %s: expected %d\n", unsigned(test.value), cls.name, expected);
            }
            assert(bool(cls.wide(test.value)) == expected);
            assert(bool(cls.wideLocale(test.value, locale)) == expected);
            assert(bool(iswctype(test.value, wctype(cls.name))) == expected);
        }
    }
    assert(iswspace(0x2003) && iswprint(0x2003) && !iswgraph(0x2003));
    assert(wctype("missing") == 0 && wctrans("missing") == nullptr);
    assert(iswctype(L'A', wctype("missing")) == 0);
    assert(towctrans(L'A', wctrans("missing")) == L'A');
    assert(towlower(0xc9) == 0xe9 && towupper(0xe9) == 0xc9);
    assert(towlower_l(0xc9, locale) == 0xe9 && towupper_l(0xe9, locale) == 0xc9);
    assert(towctrans(0xc9, wctrans("tolower")) == 0xe9);
    assert(towctrans(0xe9, wctrans("toupper")) == 0xc9);
    assert(towlower(WEOF) == WEOF && towupper(WEOF) == WEOF);
    assert(strcmp(nl_langinfo(CODESET), "UTF-8") == 0);
    assert(strcmp(nl_langinfo(ABDAY_1), "Sun") == 0 && strcmp(nl_langinfo(DAY_7), "Saturday") == 0);
    assert(strcmp(nl_langinfo(ABMON_12), "Dec") == 0 && strcmp(nl_langinfo(MON_1), "January") == 0);
    assert(strcmp(nl_langinfo(RADIXCHAR), ".") == 0 && strcmp(nl_langinfo(THOUSEP), "") == 0);
    assert(strcmp(nl_langinfo(YESEXPR), "^[yY]") == 0 && strcmp(nl_langinfo(NOEXPR), "^[nN]") == 0);
    assert(strcmp(nl_langinfo_l(AM_STR, locale), "AM") == 0);
    lconv* conventions = localeconv();
    assert(strcmp(conventions->decimal_point, ".") == 0 && conventions->thousands_sep[0] == 0);
    assert(conventions->int_frac_digits == CHAR_MAX && conventions->n_sign_posn == CHAR_MAX);
    locale_t copy = duplocale(locale);
    assert(copy);
    locale_t previous = uselocale(copy);
    assert(uselocale(nullptr) == copy && uselocale(previous) == copy);
    freelocale(copy);
    freelocale(locale);
    puts("character classes: ok");
}
