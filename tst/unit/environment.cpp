//@ compile-flags: -g -D_GNU_SOURCE
//@ stdout: "environment: ok\n"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main() {
    assert(clearenv() == 0 && getenv("PIZDOS_ENV") == nullptr);
    assert(setenv("PIZDOS_ENV", "first", 0) == 0);
    assert(strcmp(getenv("PIZDOS_ENV"), "first") == 0);
    assert(setenv("PIZDOS_ENV", "ignored", 0) == 0);
    assert(strcmp(secure_getenv("PIZDOS_ENV"), "first") == 0);
    assert(secure_getenv("missing") == nullptr);
    assert(setenv("PIZDOS_ENV", "second", 1) == 0);
    assert(strcmp(getenv("PIZDOS_ENV"), "second") == 0);
    assert(getenv("") == nullptr && getenv("PIZDOS_ENV=x") == nullptr);
    assert(setenv("", "x", 1) == -1 && errno == EINVAL);
    assert(setenv("bad=name", "x", 1) == -1 && errno == EINVAL);
    assert(unsetenv("") == -1 && errno == EINVAL);
    assert(unsetenv("bad=name") == -1 && errno == EINVAL);
    assert(unsetenv("missing") == 0);
#ifdef __wasm__
    char emptyName[] = "=value";
    assert(putenv(emptyName) == -1 && errno == EINVAL);
#endif
    assert(unsetenv("PIZDOS_ENV") == 0 && getenv("PIZDOS_ENV") == nullptr);
    char variable[] = "PIZDOS_COPY", value[] = "before";
    assert(setenv(variable, value, 1) == 0);
    variable[0] = 'X';
    value[0] = 'X';
    assert(strcmp(getenv("PIZDOS_COPY"), "before") == 0);
    char pair[] = "PIZDOS_PUT=before";
    assert(putenv(pair) == 0 && strcmp(getenv("PIZDOS_PUT"), "before") == 0);
    strcpy(strchr(pair, '=') + 1, "after");
    assert(strcmp(getenv("PIZDOS_PUT"), "after") == 0);
    char remove[] = "PIZDOS_PUT";
    assert(putenv(remove) == 0 && getenv("PIZDOS_PUT") == nullptr);
    char key[32], number[32];
    for (int i = 0; i < 100; ++i) {
        snprintf(key, sizeof(key), "PIZDOS_%d", i);
        snprintf(number, sizeof(number), "%d", i);
        assert(setenv(key, number, 1) == 0);
    }
    for (int i = 0; i < 100; ++i) {
        snprintf(key, sizeof(key), "PIZDOS_%d", i);
        snprintf(number, sizeof(number), "%d", i);
        assert(strcmp(getenv(key), number) == 0);
    }
    assert(clearenv() == 0 && getenv("PIZDOS_99") == nullptr);
    assert(clearenv() == 0);
    char supplied[] = "PIZDOS_SUPPLIED=one";
    char* suppliedEnvironment[] = {supplied, nullptr};
    environ = suppliedEnvironment;
    assert(strcmp(getenv("PIZDOS_SUPPLIED"), "one") == 0);
    assert(setenv("PIZDOS_ADDED", "new", 1) == 0);
    strcpy(strchr(supplied, '=') + 1, "two");
    assert(strcmp(getenv("PIZDOS_SUPPLIED"), "two") == 0);
    assert(strcmp(getenv("PIZDOS_ADDED"), "new") == 0);
    assert(unsetenv("PIZDOS_SUPPLIED") == 0 && getenv("PIZDOS_SUPPLIED") == nullptr);
    assert(clearenv() == 0);

    char options[] = "one=1,on,one,nope=4,,two=";
    char on[] = "on", one[] = "one", two[] = "two";
    char* keys[] = {on, one, two, nullptr};
    char* next = options;
    char* argument;
    assert(getsubopt(&next, keys, &argument) == 1 && strcmp(argument, "1") == 0);
    assert(getsubopt(&next, keys, &argument) == 0 && argument == nullptr);
    assert(getsubopt(&next, keys, &argument) == 1 && argument == nullptr);
    assert(getsubopt(&next, keys, &argument) == -1);
    assert(getsubopt(&next, keys, &argument) == -1);
    assert(getsubopt(&next, keys, &argument) == 2 && argument && argument[0] == 0 && next[0] == 0);
    puts("environment: ok");
}
