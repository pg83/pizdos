//@ env: LC_ALL=C
//@ compile-flags: -g -D_GNU_SOURCE
//@ stdout: "filename patterns: ok\n"

#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>

int main() {
    struct Match {
        const char* pattern;
        const char* text;
        int flags;
        bool matches;
    };
    const Match cases[] = {
        {"", "", 0, true},
        {"", "a", 0, false},
        {"a", "", 0, false},
        {"abc", "abc", 0, true},
        {"abc", "abd", 0, false},
        {"*", "", 0, true},
        {"a***b*c", "axybzc", 0, true},
        {"a*b", "ab", 0, true},
        {"a*b", "ac", 0, false},
        {"?", "a", 0, true},
        {"?", "", 0, false},
        {"?", "ab", 0, false},
        {"a\\*b", "a*b", 0, true},
        {"a\\?b", "a?b", 0, true},
        {"a\\*b", "axxb", 0, false},
        {"a\\*b", "a\\xxb", FNM_NOESCAPE, true},
        {"[abc]", "b", 0, true},
        {"[abc]", "x", 0, false},
        {"[!abc]", "x", 0, true},
        {"[!abc]", "a", 0, false},
        {"[a-z]", "m", 0, true},
        {"[a-z]", "M", 0, false},
        {"[]a]", "]", 0, true},
        {"[-a]", "-", 0, true},
        {"[a-]", "-", 0, true},
        {"[", "[", 0, true},
        {"[abc", "[abc", 0, true},
        {"[abc", "xabc", 0, false},
        {"*", "a/b", 0, true},
        {"*", "a/b", FNM_PATHNAME, false},
        {"?", "/", FNM_PATHNAME, false},
        {"[/]", "/", FNM_PATHNAME, false},
        {"a/*/c", "a/b/c", FNM_PATHNAME, true},
        {"a/*/c", "a/b/d/c", FNM_PATHNAME, false},
        {"*", ".hidden", 0, true},
        {"*", ".hidden", FNM_PERIOD, false},
        {"?hidden", ".hidden", FNM_PERIOD, false},
        {"[.]hidden", ".hidden", FNM_PERIOD, false},
        {".*", ".hidden", FNM_PERIOD, true},
        {"a/*", "a/.hidden", FNM_PATHNAME | FNM_PERIOD, false},
        {"a/.*", "a/.hidden", FNM_PATHNAME | FNM_PERIOD, true},
        {"a/?", "a/.", FNM_PATHNAME | FNM_PERIOD, false},
        {"a/[.]", "a/.", FNM_PATHNAME | FNM_PERIOD, false},
        {"abc", "ABC", FNM_CASEFOLD, true},
        {"[a-z]", "M", FNM_CASEFOLD, true},
        {"[A-Z]", "m", FNM_CASEFOLD, true},
        {"[[:upper:]]", "m", FNM_CASEFOLD, true},
        {"abc", "abc/def", FNM_LEADING_DIR, true},
        {"abc", "abcd/def", FNM_LEADING_DIR, false},
        {"[[:alnum:]]", "7", 0, true},
        {"[[:alpha:]]", "x", 0, true},
        {"[[:blank:]]", "\t", 0, true},
        {"[[:cntrl:]]", "\n", 0, true},
        {"[[:digit:]]", "7", 0, true},
        {"[[:graph:]]", "!", 0, true},
        {"[[:lower:]]", "x", 0, true},
        {"[[:print:]]", " ", 0, true},
        {"[[:punct:]]", "!", 0, true},
        {"[[:space:]]", "\r", 0, true},
        {"[[:upper:]]", "X", 0, true},
        {"[[:xdigit:]]", "f", 0, true},
        {"[![:digit:]]", "7", 0, false},
        {"[[:digit:]]", "x", 0, false},
    };
    for (const auto& entry : cases) {
        bool matches = fnmatch(entry.pattern, entry.text, entry.flags) == 0;
        if (matches != entry.matches) {
            fprintf(stderr, "fnmatch('%s', '%s', %d): got %d, expected %d\n", entry.pattern, entry.text, entry.flags, matches, entry.matches);
            abort();
        }
    }
    puts("filename patterns: ok");
}
