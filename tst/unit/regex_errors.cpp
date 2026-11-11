//@ compile-flags: -g
//@ stdout: "regex errors: ok\n"

#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct Case {
    const char* pattern;
    int flags;
    int expected;
    int alternate = -1;
};

int main() {
    const Case cases[] = {
        {"\\", 0, REG_EESCAPE},
        {"[", 0, REG_EBRACK},
        // This has both an invalid class and an unclosed bracket; either
        // diagnostic is valid, but the expression must be rejected.
        {"[[:alpha]", 0, REG_EBRACK, REG_ECTYPE},
        {"[[:unknown:]]", 0, REG_ECTYPE},
        {"\\)", 0, REG_EPAREN},
        {"\\(", 0, REG_EPAREN},
        {"(", REG_EXTENDED, REG_EPAREN},
        {"a\\{1", 0, REG_EBRACE, REG_BADBR},
        {"\\{1\\}", 0, REG_BADRPT},
        {"a\\{x\\}", 0, REG_BADBR},
        {"a\\{2,1\\}", 0, REG_BADBR},
        {"\\1", 0, REG_ESUBREG},
        {"[z-a]", REG_EXTENDED, REG_ERANGE},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        regex_t expression;
        int result = regcomp(&expression, cases[i].pattern, cases[i].flags);
        if (result == 0 || (result != cases[i].expected && result != cases[i].alternate)) {
            fprintf(stderr, "case %u: regcomp returned %d, expected %d\n", i, result, cases[i].expected);
            abort();
        }
        char message[256];
        size_t needed = regerror(result, &expression, nullptr, 0);
        if (needed < 2 || needed > sizeof(message) || regerror(result, &expression, message, sizeof(message)) != needed || strlen(message) + 1 != needed) {
            abort();
        }
        char shortMessage[] = "xyz";
        if (regerror(result, &expression, shortMessage, 1) != needed || shortMessage[0] != 0 || shortMessage[1] != 'y') {
            abort();
        }
    }
    puts("regex errors: ok");
    return 0;
}
