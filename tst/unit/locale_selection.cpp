//@ host: false
//@ compile-flags: -g -D_GNU_SOURCE -fno-builtin
//@ stdout: "locale selection: ok\n"

#include <assert.h>
#include <langinfo.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main() {
    assert(unsetenv("LC_ALL") == 0 && unsetenv("LC_CTYPE") == 0 && setenv("LANG", "C", 1) == 0);
    assert(strcmp(setlocale(LC_CTYPE, ""), "C") == 0);
    assert(setenv("LC_CTYPE", "C.UTF-8", 1) == 0);
    assert(strcmp(setlocale(LC_CTYPE, ""), "C.UTF-8") == 0);
    assert(setenv("LC_ALL", "C", 1) == 0);
    assert(strcmp(setlocale(LC_CTYPE, ""), "C") == 0);
    assert(unsetenv("LC_ALL") == 0 && unsetenv("LC_CTYPE") == 0);
    assert(setenv("LANG", "test.UTF-8", 1) == 0);
    assert(strcmp(setlocale(LC_CTYPE, ""), "test.UTF-8") == 0);
    assert(strcmp(nl_langinfo(_NL_LOCALE_NAME(LC_CTYPE)), "test.UTF-8") == 0);
    assert(strcmp(nl_langinfo(0x7fffffff), "") == 0);
    assert(setlocale(-1, "C") == nullptr && setlocale(LC_ALL + 1, nullptr) == nullptr);
    assert(strcmp(setlocale(LC_CTYPE, nullptr), "test.UTF-8") == 0);
    const char* invalidNames[] = {".hidden", "/not/a/locale"};
    for (const char* name : invalidNames) {
        assert(strcmp(setlocale(LC_CTYPE, name), "C.UTF-8") == 0);
    }
    assert(strcmp(setlocale(LC_CTYPE, "test.UTF-8"), "test.UTF-8") == 0);
    assert(setlocale(LC_CTYPE, "test.ISO-8859-1") == nullptr);
    assert(strcmp(setlocale(LC_CTYPE, nullptr), "test.UTF-8") == 0);
    assert(setenv("LANG", "C", 1) == 0 && strcmp(setlocale(LC_ALL, ""), "C") == 0);
    puts("locale selection: ok");
}
