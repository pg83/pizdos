//@ host: false
//@ compile-flags: -g -fno-builtin
//@ stdout: "regex extensions: ok\n"

#include <assert.h>
#include <regex.h>
#include <stdio.h>

// GNU escapes supplement POSIX syntax in the kernel's regex interface;
// the host's musl regex need not implement those extensions.
struct Case {
    const char* pattern;
    const char* text;
    int flags;
    int begin;
    int end;
};

int main() {
    const Case cases[] = {
        {R"(a\|bc)", "xbc", 0, 1, 3},
        {R"(a\|bc)", "xa", 0, 1, 2},
        {R"(a\+)", "xaaab", 0, 1, 4},
        {R"(a\?)", "a", 0, 0, 1},
        {R"(a\?)", "b", 0, 0, 0},
        {R"(\+)", "+", 0, 0, 1},
        {R"(\?)", "?", 0, 0, 1},
        {R"(\<word\>)", "a word!", REG_EXTENDED, 2, 6},
        {R"(\bword\b)", "sword word!", REG_EXTENDED, 6, 10},
        {R"(\Boo\B)", "food", REG_EXTENDED, 1, 3},
        {R"(\w\W\s\S)", "a! b", REG_EXTENDED, 0, 4},
        {R"(\`abc\')", "abc", REG_EXTENDED, 0, 3},
        {R"(\`abc\')", "xabc", REG_EXTENDED, -1, -1},
        {R"(\`abc\')", "abcx", REG_EXTENDED, -1, -1},
        {R"(\n\t)", "x\n\ty", REG_EXTENDED, 1, 3},
        {R"(\q)", "aqb", REG_EXTENDED, 1, 2},
        {R"(\(ab\)\{2\})", "xababz", 0, 1, 5},
        {R"(a\{2,\})", "xaaab", 0, 1, 4},
        {R"(a\{0,2\})", "aaab", 0, 0, 2},
        {"a{2}", "xaaab", REG_EXTENDED, 1, 3},
        {"a{2,}", "xaaab", REG_EXTENDED, 1, 4},
        {"a{0,2}", "aaab", REG_EXTENDED, 0, 2},
        {"(ab){2,3}", "xabababz", REG_EXTENDED, 1, 7},
        {"a**", "xaaab", REG_EXTENDED, 0, 0},
        {"a*+", "aaab", REG_EXTENDED, 0, 3},
        {"a+?", "aaab", REG_EXTENDED, 0, 3},
        {")", "x)y", REG_EXTENDED, 1, 2},
    };
    for (const auto& entry : cases) {
        regex_t expression{};
        int code = regcomp(&expression, entry.pattern, entry.flags);
        assert(code == 0);
        regmatch_t matches[4];
        code = regexec(&expression, entry.text, 4, matches, 0);
        if (entry.begin < 0) {
            assert(code == REG_NOMATCH);
        } else {
            assert(code == 0 && matches[0].rm_so == entry.begin && matches[0].rm_eo == entry.end);
        }
        regfree(&expression);
    }
    puts("regex extensions: ok");
}
