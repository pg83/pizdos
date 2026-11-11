//@ env: TZ=UTC0
//@ compile-flags: -g -D_GNU_SOURCE
//@ stdout: "wide strings: ok\n"

#include <assert.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <wchar.h>

int main() {
    assert(setlocale(LC_ALL, "C.UTF-8") != nullptr);
    wchar_t source[] = L"abéab";
    wchar_t result[32];
    assert(wcslen(source) == 5 && wcsnlen(source, 3) == 3 && wcsnlen(source, 0) == 0);
    assert(wcscpy(result, source) == result && wcscmp(result, source) == 0);
    assert(wcscat(result, L"c") == result && wcscmp(result, L"abéabc") == 0);
    assert(wcsncpy(result, L"x", 4) == result && result[0] == L'x' && result[1] == 0 && result[3] == 0);
    assert(wcsncpy(result, L"abcd", 3) == result && wmemcmp(result, L"abc", 3) == 0);
    assert(wcsncmp(L"a", L"b", 0) == 0 && wcsncmp(L"abc", L"abd", 2) == 0);
    assert(wcsncmp(L"abc", L"abd", 3) < 0 && wcsncmp(L"abd", L"abc", 3) > 0);
    assert(wcsncmp(L"a", L"a", 99) == 0 && wcscmp(L"ab", L"a") > 0);
    assert(wcscoll(L"a", L"b") < 0 && wcscoll(L"b", L"a") > 0 && wcscoll(L"a", L"a") == 0);
    assert(wcsxfrm(nullptr, L"abc", 0) == 3);
    assert(wcsxfrm(result, L"abc", 32) == 3 && wcscmp(result, L"abc") == 0);
    assert(wcsxfrm(result, L"abc", 1) == 3);
    assert(wcschr(source, L'é') == source + 2 && wcschr(source, 0) == source + 5 && wcschr(source, L'x') == nullptr);
    assert(wcsrchr(source, L'a') == source + 3 && wcsrchr(source, 0) == source + 5 && wcsrchr(source, L'x') == nullptr);
    assert(wcsstr(source, L"éa") == source + 2 && wcsstr(source, L"") == source && wcsstr(source, L"xx") == nullptr);
    wchar_t aligned[] = {0x100, 0x100, 0x100, 0}, absent[] = {1, 1, 0};
    assert(wcsstr(aligned, absent) == nullptr);
    assert(wcsspn(source, L"abc") == 2 && wcscspn(source, L"é") == 2);
    assert(wcsspn(source, L"abé") == 5 && wcscspn(source, L"x") == 5);
    assert(wcspbrk(source, L"xé") == source + 2 && wcspbrk(source, L"xy") == nullptr);
    wchar_t tokens[] = L",a::b,c,";
    wchar_t* next = nullptr;
    assert(wcscmp(wcstok(tokens, L",:", &next), L"a") == 0);
    assert(wcscmp(wcstok(nullptr, L",:", &next), L"b") == 0);
    assert(wcscmp(wcstok(nullptr, L",:", &next), L"c") == 0);
    assert(wcstok(nullptr, L",:", &next) == nullptr && wcstok(nullptr, L",:", &next) == nullptr);
    wchar_t* copy = wcsdup(source);
    assert(copy && copy != source && wcscmp(copy, source) == 0);
    free(copy);
    assert(wmemchr(source, L'é', 5) == source + 2 && wmemchr(source, L'x', 5) == nullptr);
    assert(wmemchr(source, 0, 6) == source + 5);
    assert(wmemcmp(L"a", L"b", 1) < 0 && wmemcmp(L"b", L"a", 1) > 0);
    assert(wmemcmp(source, source, 5) == 0 && wmemcmp(L"a", L"b", 0) == 0);
    assert(wmemcpy(result, source, 6) == result && wcscmp(result, source) == 0);
    assert(wmemmove(result + 1, result, 6) == result + 1 && wcscmp(result, L"aabéab") == 0);
    assert(wmemset(result, L'x', 3) == result && wmemcmp(result, L"xxx", 3) == 0);
    struct Width {
        wchar_t character;
        int columns;
    };
    const Width widths[] = {{0, 0}, {L'A', 1}, {L'\n', -1}, {0x7f, -1}, {L'é', 1}, {L'Ā', 1}, {0x301, 0}, {L'中', 2}, {0x20000, 2}, {0x40000, 1}, {0xe0020, 0}, {0xe1000, 1}, {0xffff, -1}};
    for (const auto& width : widths) {
        assert(wcwidth(width.character) == width.columns);
    }
    assert(wcswidth(L"a中é\u0301", 99) == 4 && wcswidth(L"a中é", 2) == 3);
    assert(wcswidth(L"a\nb", 3) == -1 && wcswidth(L"a", 0) == 0);
    time_t epoch = 0;
    tm value{};
    assert(gmtime_r(&epoch, &value) == &value);
    assert(wcsftime(result, 32, L"%Y年%m月%d日", &value) == 11);
    assert(wcscmp(result, L"1970年01月01日") == 0);
    assert(wcsftime(result, 4, L"%Y", &value) == 0);
    assert(wcsftime(result, 0, L"%Y", &value) == 0);
#ifdef __wasm__
    const wchar_t malformed[] = {wchar_t(0x110000), wchar_t(0xd800), 0};
    assert(wcsftime(result, 32, malformed, &value) == 2);
    assert(result[0] == 0xfffd && result[1] == 0xfffd && result[2] == 0);
#endif
    puts("wide strings: ok");
}
