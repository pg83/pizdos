//@ compile-flags: -g -D_GNU_SOURCE -fno-builtin
//@ env: TZ=UTC0
//@ host: false
//@ stdout: "calendar extensions: ok\n"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static void formatted(const tm& value, const char* format, const char* expected) {
    char text[128];
    assert(strftime(text, sizeof(text), format, &value) == strlen(expected));
    assert(strcmp(text, expected) == 0);
}

int main() {
    const char* inputs[] = {"-1 tail", "+0 tail", "951782400 tail"};
    const time_t seconds[] = {-1, 0, 951782400};
    for (unsigned i = 0; i != 3; ++i) {
        tm value{}, expected{};
        assert(strptime(inputs[i], "%s", &value) == inputs[i] + strlen(inputs[i]) - 5);
        assert(gmtime_r(&seconds[i], &expected) == &expected);
        assert(value.tm_year == expected.tm_year && value.tm_mon == expected.tm_mon && value.tm_mday == expected.tm_mday);
        assert(value.tm_hour == expected.tm_hour && value.tm_min == expected.tm_min && value.tm_sec == expected.tm_sec);
        assert(value.tm_yday == expected.tm_yday && value.tm_wday == expected.tm_wday);
    }
    tm value{};
    const char* leap = "2024-02-29 rest";
    assert(strptime(leap, "%F", &value) == leap + 10);
    assert(value.tm_year == 124 && value.tm_mon == 1 && value.tm_mday == 29);
    assert(strptime(leap, "%EY-%Om-%Od", &value) == leap + 10);
    time_t epoch = 0;
    assert(gmtime_r(&epoch, &value) == &value);
    formatted(value, "%^A|%10B|%P", "THURSDAY|   January|am");
    formatted(value, "%k|%l|%s|%Q", " 0|12|0|%Q");
    value.tm_hour = 13;
    formatted(value, "%P", "pm");
    formatted(value, "%k|%l|%s", "13| 1|46800");
    formatted(value, "prefix%_", "prefix");
    formatted(value, "prefix%E", "prefix");
    formatted(value, "prefix%O", "prefix");

    value.tm_year = 8100;
    char text[32];
    errno = 0;
    assert(asctime(&value) == nullptr && errno == EOVERFLOW);
    errno = 0;
    assert(asctime_r(&value, text) == nullptr && errno == EOVERFLOW);
    const char* malformed[] = {"%E", "%O", "%EO", "%EEEE"};
    for (const char* format : malformed) {
        value = {};
        assert(strptime("2024", format, &value) == nullptr);
    }
    value = {};
    value.tm_wday = -1;
    value.tm_mon = 13;
    const size_t length = strftime(text, sizeof(text), "%a|%A|%b|%B", &value);
    assert(length < sizeof(text) && text[length] == 0);
    puts("calendar extensions: ok");
}
