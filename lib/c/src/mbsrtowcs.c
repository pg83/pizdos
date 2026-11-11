/* mbsrtowcs converts a character at a time, by mbrtowc. musl's reads ASCII a
   word at a time, past the end of a string that ends inside a word, which
   the address sanitizer reports; the rest is musl's semantics: the count
   converted, *src at the rest or null at the end, EILSEQ and -1 for a
   sequence that is not a character. */
#include <errno.h>
#include <wchar.h>

size_t mbsrtowcs(wchar_t* restrict ws, const char** restrict src, size_t wn, mbstate_t* restrict st) {
    static mbstate_t state;
    const char* s = *src;
    size_t n = 0;

    if (!st) {
        st = &state;
    }

    for (;; n++) {
        if (ws && n == wn) {
            *src = s;
            return n;
        }

        wchar_t wc;
        size_t k = mbrtowc(&wc, s, 4, st);

        if (k == (size_t)-1 || k == (size_t)-2) {
            errno = EILSEQ;

            if (ws) {
                *src = s;
            }

            return (size_t)-1;
        }

        if (k == 0) {
            if (ws) {
                ws[n] = 0;
                *src = 0;
            }

            return n;
        }

        if (ws) {
            ws[n] = wc;
        }

        s += k;
    }
}
