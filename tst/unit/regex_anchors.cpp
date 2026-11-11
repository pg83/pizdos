//@ compile-flags: -g
//@ stdout: "regex anchors: ok\n"

#include <regex.h>
#include <stdio.h>
#include <stdlib.h>

struct Case {
    const char* pattern;
    const char* subject;
    int compileFlags;
    int executionFlags;
    int result;
};

int main() {
    const Case cases[] = {
        {"^abc$", "abc", REG_EXTENDED, 0, 0},
        {"^abc$", "abc", REG_EXTENDED, REG_NOTBOL, REG_NOMATCH},
        {"^abc$", "abc", REG_EXTENDED, REG_NOTEOL, REG_NOMATCH},
        {"^abc$", "abc", REG_EXTENDED | REG_NEWLINE, 0, 0},
        {"^abc$", "\nabc\n", REG_EXTENDED | REG_NEWLINE, REG_NOTBOL | REG_NOTEOL, 0},
        {"^abc$", "abc", REG_EXTENDED | REG_NEWLINE, REG_NOTBOL, REG_NOMATCH},
        {"^abc$", "abc", REG_EXTENDED | REG_NEWLINE, REG_NOTEOL, REG_NOMATCH},
        {"^abc$", "abc", REG_EXTENDED | REG_NEWLINE, REG_NOTBOL | REG_NOTEOL, REG_NOMATCH},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        const Case& item = cases[i];
        regex_t expression;
        if (regcomp(&expression, item.pattern, item.compileFlags) != 0) {
            abort();
        }
        int result = regexec(&expression, item.subject, 0, nullptr, item.executionFlags);
        regfree(&expression);
        if (result != item.result) {
            fprintf(stderr, "case %u: regexec returned %d, expected %d\n", i, result, item.result);
            abort();
        }
    }
    puts("regex anchors: ok");
    return 0;
}
