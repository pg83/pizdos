//@ env: TZ=UTC0
//@ compile-flags: -g
//@ stdout: "calendar formatting: ok\n"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void formatted(const tm& value, const char* format, const char* expected) {
    char result[256];
    size_t size = strftime(result, sizeof(result), format, &value);
    if (size != strlen(expected) || strcmp(result, expected) != 0) {
        fprintf(stderr, "%s: got '%s', expected '%s'\n", format, result, expected);
        abort();
    }
}

int main() {
    time_t epoch = 0;
    tm value{};
    assert(gmtime_r(&epoch, &value) == &value);
    assert(value.tm_year == 70 && value.tm_mon == 0 && value.tm_mday == 1);
    assert(value.tm_wday == 4 && value.tm_yday == 0 && value.tm_isdst == 0);
    assert(gmtime(&epoch)->tm_year == 70);
    assert(localtime(&epoch)->tm_hour == 0);
    assert(localtime_r(&epoch, &value) == &value);
    formatted(value, "%a|%A|%b|%B|%h|%c", "Thu|Thursday|Jan|January|Jan|Thu Jan  1 00:00:00 1970");
    formatted(value, "%C|%d|%D|%e|%F|%g|%G|%H|%I|%j|%m|%M", "19|01|01/01/70| 1|1970-01-01|70|1970|00|12|001|01|00");
    formatted(value, "%p|%r|%R|%S|%T|%u|%U|%V|%w|%W|%x|%X|%y|%Y|%z|%Z", "AM|12:00:00 AM|00:00|00|00:00:00|4|00|01|4|00|01/01/70|00:00:00|70|1970|+0000|UTC");
    formatted(value, "%%:%n:%t", "%:\n:\t");
    formatted(value, "%Od/%Om/%EY", "01/01/1970");
    formatted(value, "%-d|%_d|%0e|%6Y", "1| 1|01|001970");
    assert(strcmp(asctime(&value), "Thu Jan  1 00:00:00 1970\n") == 0);
    char text[32];
    assert(asctime_r(&value, text) == text && strcmp(text, asctime(&value)) == 0);
    assert(strcmp(ctime(&epoch), text) == 0);
    assert(ctime_r(&epoch, text) == text && strcmp(text, "Thu Jan  1 00:00:00 1970\n") == 0);
    char shortText[4] = {'x', 'x', 'x', 'x'};
    assert(strftime(shortText, 0, "%Y", &value) == 0 && shortText[0] == 'x');
    assert(strftime(shortText, sizeof(shortText), "%Y", &value) == 0);
    assert(strftime(shortText, sizeof(shortText), "%m", &value) == 2 && strcmp(shortText, "01") == 0);
    assert(strftime(shortText, sizeof(shortText), "", &value) == 0 && shortText[0] == 0);

    time_t beforeEpoch = -1;
    assert(gmtime_r(&beforeEpoch, &value) == &value);
    formatted(value, "%F %T %j", "1969-12-31 23:59:59 365");
    assert(difftime(epoch, beforeEpoch) == 1.0 && difftime(beforeEpoch, epoch) == -1.0);
    value = {};
    value.tm_year = 100;
    value.tm_mon = 1;
    value.tm_mday = 29;
    value.tm_hour = 23;
    value.tm_min = 59;
    value.tm_sec = 60;
    assert(timegm(&value) == 951868800);
    assert(value.tm_mon == 2 && value.tm_mday == 1 && value.tm_hour == 0 && value.tm_yday == 60);
    assert(mktime(&value) == 951868800);
    formatted(value, "%F %T %p", "2000-03-01 00:00:00 AM");
    value.tm_hour = 13;
    formatted(value, "%I %p %r", "01 PM 01:00:00 PM");
    value.tm_mon = -1;
    value.tm_mday = 32;
    value.tm_hour = 0;
    value.tm_sec = -1;
    assert(timegm(&value) == 946684799);
    formatted(value, "%F %T", "1999-12-31 23:59:59");

    const time_t overflow = (time_t(1) << 62);
    errno = 0;
    assert(gmtime_r(&overflow, &value) == nullptr && errno == EOVERFLOW);
    errno = 0;
    assert(gmtime(&overflow) == nullptr && errno == EOVERFLOW);
    puts("calendar formatting: ok");
}
