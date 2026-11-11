//@ env: LC_ALL=C
//@ compile-flags: -g -D_GNU_SOURCE
//@ stdout: "string utilities: ok\n"

#include <assert.h>
#include <errno.h>
#include <libgen.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

int main() {
    char text[] = "abcabc";
    assert(strchr(text, 'b') == text + 1 && strrchr(text, 'b') == text + 4);
    assert(strchr(text, 0) == text + 6 && strrchr(text, 0) == text + 6);
    assert(strchr(text, 'x') == nullptr && strrchr(text, 'x') == nullptr);
    assert(strchrnul(text, 'b') == text + 1 && strchrnul(text, 'x') == text + 6);
    assert(strchrnul(text, 0) == text + 6);
    assert(strpbrk(text, "xc") == text + 2 && strpbrk(text, "xy") == nullptr);
    assert(strncmp("a", "b", 0) == 0 && strncmp("abc", "abd", 2) == 0);
    assert(strncmp("abc", "abd", 3) < 0 && strncmp("ab", "a", 3) > 0);
    assert(strncmp("a", "a", 99) == 0 && strnlen(text, 3) == 3 && strnlen(text, 99) == 6);
    assert(strnlen(text, 0) == 0);
    assert(strcasecmp("AbC", "aBc") == 0 && strcasecmp("abc", "ABCd") < 0);
    assert(strncasecmp("ABC", "abD", 2) == 0 && strncasecmp("ABC", "abD", 3) < 0);
    assert(strstr(text, "cab") == text + 2 && strstr(text, "x") == nullptr && strstr(text, "") == text);
    assert(strcasestr(text, "CAB") == text + 2 && strcasestr(text, "X") == nullptr);
    char result[32];
    memset(result, 'x', sizeof(result));
    assert(stpcpy(result, "abc") == result + 3 && strcmp(result, "abc") == 0);
    assert(stpncpy(result, "q", 4) == result + 1);
    assert(result[0] == 'q' && result[1] == 0 && result[2] == 0 && result[3] == 0 && result[4] == 'x');
    assert(stpncpy(result, "abcdef", 3) == result + 3 && memcmp(result, "abc", 3) == 0);
    assert(strncpy(result, "q", 3) == result && result[0] == 'q' && result[2] == 0);
    assert(strlcpy(result, "abcdef", 4) == 6 && strcmp(result, "abc") == 0);
    assert(strlcpy(result, "x", 0) == 1 && strcmp(result, "abc") == 0);
    assert(strlcat(result, "defgh", 5) == 8 && strcmp(result, "abcd") == 0);
    assert(strlcat(result, "xy", 2) == 4 && strcmp(result, "abcd") == 0);
    assert(strncat(result, "efgh", 2) == result && strcmp(result, "abcdef") == 0);
    assert(strncat(result, "g", 10) == result && strcmp(result, "abcdefg") == 0);
    char* copy = strdup(text);
    assert(copy && strcmp(copy, text) == 0 && copy != text);
    free(copy);
    copy = strndup(text, 3);
    assert(copy && strcmp(copy, "abc") == 0);
    free(copy);
    copy = strndup(text, 0);
    assert(copy && copy[0] == 0);
    free(copy);

    assert(mempcpy(result, text, 6) == result + 6 && memcmp(result, text, 6) == 0);
    assert(memccpy(result, "abcdef", 'c', 6) == result + 3 && memcmp(result, "abc", 3) == 0);
    assert(memccpy(result, "abcdef", 'x', 6) == nullptr && memcmp(result, "abcdef", 6) == 0);
    assert(memrchr(text, 'b', 6) == text + 4 && memrchr(text, 'x', 6) == nullptr);
#ifdef __wasm__
    // The host musl does not provide this GNU extension.
    assert(rawmemchr(text, 'c') == text + 2 && rawmemchr(text, 0) == text + 6);
#endif
    assert(memmem(text, 6, "ca", 2) == text + 2 && memmem(text, 6, "", 0) == text);
    assert(memmem(text, 6, "abcd", 4) == nullptr);
    bcopy("abcdef", result, 7);
    bcopy(result, result + 1, 5);
    assert(memcmp(result, "aabcde", 6) == 0);
    bzero(result, 3);
    assert(result[0] == 0 && result[2] == 0 && result[3] == 'c');
    explicit_bzero(result, sizeof(result));
    for (char byte : result) {
        assert(byte == 0);
    }
    swab(text, result, 5);
    assert(memcmp(result, "baac", 4) == 0 && result[4] == 0);
    swab(text, result, -1);
    assert(memcmp(result, "baac", 4) == 0);

    char tokens[] = ",a::b,c,";
    char* save = nullptr;
    assert(strcmp(strtok_r(tokens, ",:", &save), "a") == 0);
    assert(strcmp(strtok_r(nullptr, ",:", &save), "b") == 0);
    assert(strcmp(strtok_r(nullptr, ",:", &save), "c") == 0);
    assert(strtok_r(nullptr, ",:", &save) == nullptr && strtok_r(nullptr, ",:", &save) == nullptr);
    strcpy(tokens, "a:b");
    assert(strcmp(strtok(tokens, ":"), "a") == 0);
    assert(strcmp(strtok(nullptr, ":"), "b") == 0 && strtok(nullptr, ":") == nullptr);
    strcpy(tokens, ":a::");
    char* next = tokens;
    assert(strcmp(strsep(&next, ":"), "") == 0 && strcmp(strsep(&next, ":"), "a") == 0);
    assert(strcmp(strsep(&next, ":"), "") == 0 && strcmp(strsep(&next, ":"), "") == 0);
    assert(next == nullptr && strsep(&next, ":") == nullptr);
    assert(strcoll("abc", "abd") < 0 && strcoll("abc", "abc") == 0);
    assert(strxfrm(nullptr, "abc", 0) == 3);
    assert(strxfrm(result, "abc", sizeof(result)) == 3 && strcmp(result, "abc") == 0);
    assert(strxfrm(result, "abcdef", 3) == 6);
    assert(strverscmp("file9", "file10") < 0 && strverscmp("file10", "file9") > 0);
    assert(strverscmp("file000", "file00") < 0 && strverscmp("file01", "file1") < 0);
    assert(strverscmp("n123", "n12") > 0 && strverscmp("n12", "n123") < 0);
    assert(strverscmp("same", "same") == 0 && strverscmp("z", "a") > 0);

    const char* paths[] = {"", "a", "/", "////", "/a/b/", "a//b", "a//b////"};
    const char* directories[] = {".", ".", "/", "/", "/a", "a", "a"};
    const char* bases[] = {".", "a", "/", "/", "b", "b", "b"};
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        strcpy(result, paths[i]);
        assert(strcmp(dirname(result), directories[i]) == 0);
        strcpy(result, paths[i]);
        assert(strcmp(basename(result), bases[i]) == 0);
    }
    assert(strcmp(dirname(nullptr), ".") == 0 && strcmp(basename(nullptr), ".") == 0);
    assert(ffs(0) == 0 && ffs(1) == 1 && ffs(-8) == 4);
    assert(ffsl(8) == 4 && ffsll(1LL << 60) == 61);
    assert(abs(-9) == 9 && abs(9) == 9 && labs(-7) == 7 && labs(7) == 7);
    assert(llabs(-123456789012LL) == 123456789012LL && llabs(7) == 7);
    assert(div(-10, 3).quot == -3 && div(-10, 3).rem == -1);
    assert(ldiv(10, -3).quot == -3 && ldiv(10, -3).rem == 1);
    assert(lldiv(-123456789012LL, 10).quot == -12345678901LL && lldiv(-123456789012LL, 10).rem == -2);
    assert(strerror(0)[0] && strerror(ENOENT)[0] && strerror(99999)[0]);
    assert(strerror_r(ENOENT, result, sizeof(result)) == 0 && result[0]);
    assert(strerror_r(ENOENT, result, 1) == ERANGE);
    assert(strerror_r(ENOENT, result, 0) == ERANGE);
    assert(strsignal(SIGINT)[0] && strsignal(SIGRTMIN)[0] && strsignal(99999)[0]);

    char utf8[] = "Äbc";
    assert(strcasestr(utf8, "ä") == nullptr);
    char invalid[] = "\xff" "Ab";
    assert(strcasestr(invalid, "ab") == invalid + 1);
    puts("string utilities: ok");
}
