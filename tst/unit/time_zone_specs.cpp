//@ compile-flags: -g -D_GNU_SOURCE -fno-builtin
//@ env: TZ=UTC0
//@ stdout: "time zone specifications: ok\n"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct Zone {
    const char* spec;
    const char* name;
    long offset;
    const char* numeric;
};

int main() {
    const Zone zones[] = {
        {"", "UTC", 0, "+0000"},
        {"UTC0", "UTC", 0, "+0000"},
        {"ABC+5:30:17", "ABC", -19817, "-0530"},
        {"XYZ-2:15:40", "XYZ", 8140, "+0215"},
        {"<+03>-3", "+03", 10800, "+0300"},
        {"<LOCAL>-0:45", "LOCAL", 2700, "+0045"},
        {"abc+0:30", "abc", -1800, "-0030"},
        {"ABC+5:", "ABC", -18000, "-0500"},
        {"ABC+5:30:", "ABC", -19800, "-0530"},
    };
    const time_t times[] = {-86401, -1, 0, 1700000000, 2147483647};
    for (int pass = 0; pass != 2; ++pass) {
        for (const auto& zone : zones) {
            assert(setenv("TZ", zone.spec, 1) == 0);
            tzset();
            assert(timezone == -zone.offset && daylight == 0);
            assert(strcmp(tzname[0], zone.name) == 0);
            for (time_t input : times) {
                time_t adjusted = input + zone.offset;
                tm actual{}, expected{};
                assert(localtime_r(&input, &actual) == &actual && gmtime_r(&adjusted, &expected) == &expected);
                assert(actual.tm_year == expected.tm_year && actual.tm_mon == expected.tm_mon && actual.tm_mday == expected.tm_mday);
                assert(actual.tm_hour == expected.tm_hour && actual.tm_min == expected.tm_min && actual.tm_sec == expected.tm_sec);
                assert(actual.tm_wday == expected.tm_wday && actual.tm_yday == expected.tm_yday);
                assert(actual.tm_gmtoff == zone.offset && actual.tm_isdst == 0);
                assert(actual.tm_zone && strcmp(actual.tm_zone, zone.name) == 0);
                char text[128];
                assert(strftime(text, sizeof(text), "%Z", &actual) == strlen(zone.name));
                assert(strcmp(text, zone.name) == 0);
                assert(strftime(text, sizeof(text), "%z", &actual) == strlen(zone.numeric));
                assert(strcmp(text, zone.numeric) == 0);
                assert(mktime(&actual) == input);
            }
        }
    }
    assert(unsetenv("TZ") == 0);
    tzset();
    time_t epoch = 0;
    tm local{};
    assert(localtime_r(&epoch, &local) == &local && mktime(&local) == epoch);
    assert(setenv("TZ", "UTC0", 1) == 0);
    tzset();
    const time_t limits[] = {INT64_MIN, INT64_MAX};
    for (time_t input : limits) {
        errno = 0;
        assert(gmtime_r(&input, &local) == nullptr && errno == EOVERFLOW);
        errno = 0;
        assert(localtime_r(&input, &local) == nullptr && errno == EOVERFLOW);
        errno = 0;
        assert(localtime(&input) == nullptr && errno == EOVERFLOW);
        char text[32];
        errno = 0;
        assert(ctime_r(&input, text) == nullptr && errno == EOVERFLOW);
        errno = 0;
        assert(ctime(&input) == nullptr && errno == EOVERFLOW);
    }
    const int years[] = {INT32_MIN, -1500000000, 1500000000, INT32_MAX};
    for (int year : years) {
        tm input{};
        input.tm_year = year;
        input.tm_mon = 2;
        input.tm_mday = 1;
        input.tm_hour = 12;
        errno = 0;
        time_t seconds = timegm(&input);
        assert(seconds != time_t(-1) && errno == 0);
        assert(gmtime_r(&seconds, &local) == &local && local.tm_year == year);
        assert(localtime_r(&seconds, &local) == &local && local.tm_year == year);
        assert(localtime(&seconds) && localtime(&seconds)->tm_year == year);
        assert(mktime(&local) == seconds);
    }
    const int directions[] = {-1, 1};
    for (int direction : directions) {
        // Normalize past either end of the signed tm_year range.
        tm input{};
        input.tm_year = direction < 0 ? INT32_MIN : INT32_MAX;
        input.tm_mon = direction < 0 ? -1 : 12;
        input.tm_mday = 1;
        tm other = input;
        errno = 0;
        assert(timegm(&input) == time_t(-1) && errno == EOVERFLOW);
        errno = 0;
        assert(mktime(&other) == time_t(-1) && errno == EOVERFLOW);
    }
    puts("time zone specifications: ok");
}
