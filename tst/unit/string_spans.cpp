//@ compile-flags: -g -D_GNU_SOURCE -fno-builtin
//@ stdout: "string spans: ok\n"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main() {
    char text[] = "abcaXYZabc";
    const char* sets[] = {"", "abc", "XYZ", "abcdXYZ", "q", "X", "bca"};
    for (size_t offset = 0; offset <= strlen(text); ++offset) {
        for (const char* set : sets) {
            size_t accepted = 0, rejected = 0;
            while (text[offset + accepted] && strchr(set, text[offset + accepted])) {
                ++accepted;
            }
            while (text[offset + rejected] && !strchr(set, text[offset + rejected])) {
                ++rejected;
            }
            assert(strspn(text + offset, set) == accepted);
            assert(strcspn(text + offset, set) == rejected);
        }
    }
    char bytes[] = {'\xff', '\x80', 'a', 0};
    char high[] = {'\x80', '\xff', 0};
    assert(strspn(bytes, high) == 2 && strcspn(bytes, "a") == 2);
    assert(strspn(bytes, "a") == 0 && strcspn(bytes, high) == 0);
    puts("string spans: ok");
}
