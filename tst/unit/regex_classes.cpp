//@ compile-flags: -g
//@ stdout: "regex classes: ok\n"

#include <regex.h>
#include <stdio.h>
#include <stdlib.h>

struct Case {
    const char* pattern;
    const char* subject;
    int flags;
    int result;
    bool collation = false;
};

int main() {
    const Case cases[] = {
        {"[[:alnum:]]", "7", REG_EXTENDED, 0},
        {"[[:alpha:]]", "a", REG_EXTENDED, 0},
        {"[[:blank:]]", "\t", REG_EXTENDED, 0},
        {"[[:cntrl:]]", "\1", REG_EXTENDED, 0},
        {"[[:digit:]]", "0", REG_EXTENDED, 0},
        {"[[:graph:]]", "!", REG_EXTENDED, 0},
        {"[[:lower:]]", "z", REG_EXTENDED, 0},
        {"[[:print:]]", " ", REG_EXTENDED, 0},
        {"[[:punct:]]", ".", REG_EXTENDED, 0},
        {"[[:space:]]", "\n", REG_EXTENDED, 0},
        {"[[:upper:]]", "Z", REG_EXTENDED, 0},
        {"[[:xdigit:]]", "f", REG_EXTENDED, 0},
        {"[^a]", "\n", REG_EXTENDED, 0},
        {"[^a]", "\n", REG_EXTENDED | REG_NEWLINE, REG_NOMATCH},
        {"[]a]", "]", REG_EXTENDED, 0},
        {"[a-c]+", "cab", REG_EXTENDED, 0},
        {"[A-C]", "b", REG_EXTENDED | REG_ICASE, 0},
        {"[[.a.]]", "a", REG_EXTENDED, 0, true},
        {"[[=a=]]", "a", REG_EXTENDED, 0, true},
        {"[[.-.]]", "-", REG_EXTENDED, 0, true},
        {"[[.é.]]", "é", REG_EXTENDED, 0, true},
        {"\\(ab\\)\\{2,3\\}", "ababab", 0, 0},
        {"a+b", "a+b", 0, 0},
        {"a+b", "aaab", REG_EXTENDED, 0},
        {"a^b", "a^b", 0, 0},
        {"a$b", "a$b", 0, 0},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        regex_t expression;
        int compiled = regcomp(&expression, cases[i].pattern, cases[i].flags);
        // Some native libcs reject collating elements. An accepted element
        // must still match the character it names.
        if (compiled == REG_ECOLLATE && cases[i].collation) {
            continue;
        }
        if (compiled != 0) {
            fprintf(stderr, "case %u: regcomp returned %d\n", i, compiled);
            abort();
        }
        int result = regexec(&expression, cases[i].subject, 0, nullptr, 0);
        regfree(&expression);
        if (result != cases[i].result) {
            fprintf(stderr, "case %u: regexec returned %d, expected %d\n", i, result, cases[i].result);
            abort();
        }
    }
    const char* invalidCollations[] = {"[[.ab.]]", "[[==]]"};
    for (const char* pattern : invalidCollations) {
        regex_t expression;
        if (regcomp(&expression, pattern, REG_EXTENDED) != REG_ECOLLATE) {
            abort();
        }
    }
    puts("regex classes: ok");
    return 0;
}
