//@ compile-flags: -g -D_GNU_SOURCE -fno-builtin
//@ env: ZONEINFO=$ROOT/zones
//@ env: TZ=UTC0
//@ stdout: "named time zones: ok\n"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static void bigEndian(FILE* file, uint64_t value, unsigned count) {
    while (count) {
        assert(fputc((value >> (--count * 8)) & 255, file) != EOF);
    }
}

static void header(FILE* file, unsigned transitions) {
    assert(fwrite("TZif2", 1, 5, file) == 5);
    for (int i = 0; i != 15; ++i) {
        assert(fputc(0, file) != EOF);
    }
    const unsigned counts[] = {0, 0, 0, transitions, 2, 10};
    for (unsigned value : counts) {
        bigEndian(file, value, 4);
    }
}

static void types(FILE* file, bool south) {
    for (unsigned type = 0; type != 2; ++type) {
        bigEndian(file, uint32_t((south ? 36000 : -18000) + int(type) * 3600), 4);
        assert(fputc(type, file) != EOF && fputc(type * 5, file) != EOF);
    }
    assert(fwrite(south ? "SSTD\0SDST" : "NSTD\0NDST", 1, 10, file) == 10);
}

static time_t utc(int year, int month, int day, int hour = 0) {
    tm value{};
    value.tm_year = year - 1900;
    value.tm_mon = month - 1;
    value.tm_mday = day;
    value.tm_hour = hour;
    return timegm(&value);
}

static void zoneFile(const char* name, bool south, int year) {
    FILE* file = fopen(name, "wb");
    assert(file);
    // A TZif2 file includes a legacy 32-bit block followed by the 64-bit
    // block. Its transitions bracket the year being tested even past 2038.
    header(file, 0);
    types(file, south);
    header(file, 6);
    for (int y = year - 1; y <= year + 1; ++y) {
        bigEndian(file, uint64_t(utc(y, 3, 1)), 8);
        bigEndian(file, uint64_t(utc(y, 11, 1)), 8);
    }
    for (int i = 0; i != 6; ++i) {
        assert(fputc((i + (south ? 1 : 0)) % 2 == 0, file) != EOF);
    }
    types(file, south);
    const char* footer = south ? "\nSSTD-10SDST-11,M11.1.0/0,M3.1.0/0\n" : "\nNSTD5NDST4,M3.1.0/0,M11.1.0/0\n";
    assert(fwrite(footer, 1, strlen(footer), file) == strlen(footer) && fclose(file) == 0);
}

int main() {
    time_t now = time(nullptr);
    tm calendar{};
    assert(gmtime_r(&now, &calendar) == &calendar);
    int year = calendar.tm_year + 1900;
    assert(mkdir("zones", 0700) == 0);
    zoneFile("zones/north", false, year);
    zoneFile("zones/south", true, year);
    char directory[4096];
    assert(getcwd(directory, sizeof(directory)) == directory);
    for (unsigned hemisphere = 0; hemisphere != 2; ++hemisphere) {
        bool south = hemisphere != 0;
        const char* name = south ? "south" : "north";
#ifdef __wasm__
        assert(setenv("TZ", name, 1) == 0);
#else
        char path[8192];
        assert(snprintf(path, sizeof(path), ":%s/zones/%s", directory, name) > 0);
        assert(setenv("TZ", path, 1) == 0);
#endif
        tzset();
        assert(timezone == (south ? -36000 : 18000) && daylight != 0);
        assert(strcmp(tzname[0], south ? "SSTD" : "NSTD") == 0);
        assert(strcmp(tzname[1], south ? "SDST" : "NDST") == 0);
        const int years[] = {year, 2040, 2100};
        for (int sample : years) {
            for (int month = 1; month <= 7; month += 6) {
                time_t input = utc(sample, month, 15, 12);
                tm value{};
                assert(localtime_r(&input, &value) == &value);
                bool summer = south ? month == 1 : month == 7;
                int offset = (south ? 36000 : -18000) + (summer ? 3600 : 0);
                assert(value.tm_gmtoff == offset && bool(value.tm_isdst) == summer);
                assert(value.tm_hour == 12 + offset / 3600 && value.tm_mday == 15);
                assert(strcmp(value.tm_zone, south ? (summer ? "SDST" : "SSTD") : (summer ? "NDST" : "NSTD")) == 0);
                assert(mktime(&value) == input);
            }
        }
    }
    puts("named time zones: ok");
}
