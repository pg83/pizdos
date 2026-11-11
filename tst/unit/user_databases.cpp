//@ host: false
//@ compile-flags: -g -D_GNU_SOURCE -fno-builtin
//@ stdout: "user databases: ok\n"

#include <assert.h>
#include <errno.h>
#include <glob.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static void fixture(const char* path, const char* text) {
    FILE* file = fopen(path, "w");
    assert(file && fputs(text, file) >= 0 && fclose(file) == 0);
}

int main() {
    assert(mkdir("/etc", 0700) == 0 && mkdir("/tmp/test-home", 0700) == 0);
    fixture("/etc/passwd", "malformed\nbad:x:no:20:Bad:/tmp:/bin/sh\ntest:x:1234:100:Test User:/tmp/test-home:/bin/sh\nother:*:4321:200:Other:/tmp:/bin/false\n");
    fixture("/etc/group", "malformed\nbad:x:invalid:test\nbase:x:100:test\nshared:x:200:test,other\nduplicate:x:200:test\nextra:x:300:test\n");
    passwd* user = getpwuid(1234);
    assert(user && strcmp(user->pw_name, "test") == 0 && user->pw_gid == 100);
    assert(strcmp(user->pw_gecos, "Test User") == 0 && strcmp(user->pw_dir, "/tmp/test-home") == 0);
    assert(getpwuid(9999) == nullptr);
    passwd password{}, *passwordResult = reinterpret_cast<passwd*>(1);
    char passwordBuffer[256];
    assert(getpwnam_r("test", &password, passwordBuffer, 1, &passwordResult) == ERANGE && passwordResult == nullptr);
    assert(getpwnam_r("test", &password, passwordBuffer, sizeof(passwordBuffer), &passwordResult) == 0);
    assert(passwordResult == &password && password.pw_uid == 1234 && strcmp(password.pw_name, "test") == 0);
    assert(getpwnam_r("missing", &password, passwordBuffer, sizeof(passwordBuffer), &passwordResult) == 0 && passwordResult == nullptr);
    group* entry = getgrgid(200);
    assert(entry && strcmp(entry->gr_name, "shared") == 0);
    assert(strcmp(entry->gr_mem[0], "test") == 0 && strcmp(entry->gr_mem[1], "other") == 0 && entry->gr_mem[2] == nullptr);
    assert(getgrgid(9999) == nullptr);
    group record{}, *result = reinterpret_cast<group*>(1);
    char buffer[128];
    assert(getgrgid_r(200, &record, buffer, 1, &result) == ERANGE && result == nullptr);
    assert(getgrgid_r(200, &record, buffer, sizeof(buffer), &result) == 0 && result == &record);
    assert(strcmp(record.gr_name, "shared") == 0 && record.gr_gid == 200);
    assert(getgrgid_r(9999, &record, buffer, sizeof(buffer), &result) == 0 && result == nullptr);
    assert(getgrnam_r("shared", &record, buffer, 1, &result) == ERANGE && result == nullptr);
    assert(getgrnam_r("shared", &record, buffer, sizeof(buffer), &result) == 0 && result == &record);
    assert(record.gr_gid == 200 && strcmp(record.gr_mem[1], "other") == 0 && record.gr_mem[2] == nullptr);
    assert(getgrnam_r("missing", &record, buffer, sizeof(buffer), &result) == 0 && result == nullptr);
    setgrent();
    const char* names[] = {"base", "shared", "duplicate", "extra"};
    for (const char* name : names) {
        entry = getgrent();
        assert(entry && strcmp(entry->gr_name, name) == 0);
    }
    assert(getgrent() == nullptr);
    setgrent();
    assert(strcmp(getgrent()->gr_name, "base") == 0);
    endgrent();
    assert(strcmp(getgrent()->gr_name, "base") == 0);
    endgrent();

    gid_t groups[] = {999, 999, 999, 999};
    int count = 0;
    assert(getgrouplist("test", 100, nullptr, &count) == -1 && count == 3);
    count = 1;
    assert(getgrouplist("test", 100, groups, &count) == -1 && count == 3);
    assert(groups[0] == 100 && groups[1] == 999 && groups[2] == 999 && groups[3] == 999);
    count = 4;
    assert(getgrouplist("test", 100, groups, &count) == 3 && count == 3);
    assert(groups[0] == 100 && groups[1] == 200 && groups[2] == 300 && groups[3] == 999);
    count = 4;
    assert(getgrouplist("unknown", 777, groups, &count) == 1 && count == 1 && groups[0] == 777);

    glob_t paths{};
    assert(glob("~test", GLOB_TILDE | GLOB_NOESCAPE, nullptr, &paths) == 0 && paths.gl_pathc == 1);
    assert(strcmp(paths.gl_pathv[0], "/tmp/test-home") == 0);
    globfree(&paths);
    paths = {};
    assert(glob("~test/", GLOB_TILDE, nullptr, &paths) == 0 && paths.gl_pathc == 1);
    assert(strcmp(paths.gl_pathv[0], "/tmp/test-home/") == 0);
    globfree(&paths);
    // A database path can exist while being unreadable as a file.
    assert(remove("/etc/group") == 0 && mkdir("/etc/group", 0700) == 0);
    assert(getgrgid(200) == nullptr);
    count = 4;
    assert(getgrouplist("test", 100, groups, &count) == 1 && count == 1 && groups[0] == 100);
    assert(remove("/etc/passwd") == 0 && mkdir("/etc/passwd", 0700) == 0);
    assert(getpwuid(1234) == nullptr);
    puts("user databases: ok");
}
