//@ env: TZ=UTC0
//@ compile-flags: -g
//@ stdout: "calendar parsing: ok\n"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static tm parsed(const char* input, const char* format, const char* tail = "") {
    tm value{};
    const char* end = strptime(input, format, &value);
    if (end != input + strlen(input) - strlen(tail)) {
        fprintf(stderr, "strptime('%s', '%s'): unexpected remainder '%s'\n", input, format, end ? end : "<null>");
        abort();
    }
    assert(strcmp(end, tail) == 0);
    return value;
}

int main() {
    tm value = parsed("2024-02-29T23:59:60 tail", "%Y-%m-%dT%H:%M:%S", " tail");
    assert(value.tm_year == 124 && value.tm_mon == 1 && value.tm_mday == 29);
    assert(value.tm_hour == 23 && value.tm_min == 59 && value.tm_sec == 60);
    value = parsed("jAnUaRy Thursday  7 366", "%B %A %e %j");
    assert(value.tm_mon == 0 && value.tm_wday == 4 && value.tm_mday == 7 && value.tm_yday == 365);
    value = parsed("Dec Sun", "%h %a");
    assert(value.tm_mon == 11 && value.tm_wday == 0);
    value = parsed("Feb", "%b");
    assert(value.tm_mon == 1);
    value = parsed("12:01:02 AM", "%I:%M:%S %p");
    assert(value.tm_hour == 0 && value.tm_min == 1 && value.tm_sec == 2);
    value = parsed("01:02:03 pm", "%I:%M:%S %p");
    assert(value.tm_hour == 13 && value.tm_min == 2 && value.tm_sec == 3);
    value = parsed("02/29/24", "%D");
    assert(value.tm_year == 124 && value.tm_mon == 1 && value.tm_mday == 29);
    value = parsed("2024-02-29 14:23:45 06:07", "%Y-%m-%d %T %R");
    assert(value.tm_year == 124 && value.tm_mon == 1 && value.tm_mday == 29);
    assert(value.tm_hour == 6 && value.tm_min == 7 && value.tm_sec == 45);
    value = parsed("\t \n29\t % \n02", " %d%t%%%n%m");
    assert(value.tm_mday == 29 && value.tm_mon == 1);
    value = parsed("68", "%y");
    assert(value.tm_year == 168);
    value = parsed("69", "%y");
    assert(value.tm_year == 69);

    struct Invalid {
        const char* input;
        const char* format;
    };
    const Invalid invalid[] = {{"00", "%m"}, {"13", "%m"}, {"32", "%d"}, {"24", "%H"}, {"00", "%I"}, {"60", "%M"}, {"62", "%S"}, {"000", "%j"}, {"367", "%j"}, {"NotAMonth", "%B"}, {"NotADay", "%A"}, {"11:00 QQ", "%I:%M %p"}, {"11:00 A", "%I:%M %p"}, {"2024/01/01", "%Y-%m-%d"}, {"", "%Y"}, {"!", "%%"}};
    for (const auto& entry : invalid) {
        value = {};
        assert(strptime(entry.input, entry.format, &value) == nullptr);
    }

    value = parsed("19 20", "%C %y");
    assert(value.tm_year == 20);
    value = parsed("20 19", "%y %C");
    assert(value.tm_year == 20);
    value = parsed("20 68", "%C %y");
    assert(value.tm_year == 168);
    puts("calendar parsing: ok");
}
