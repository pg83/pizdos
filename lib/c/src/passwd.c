#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The user and group files read through stdio, as a program's own copy of
 * /etc/passwd or /etc/group: a line with the wrong number of fields is
 * skipped. The results are static, as getpwent's.
 */

static char* line;
static size_t lineSize;

// fields splits a line at its colons into count fields, or fails.
static int fields(char* text, char** out, int count) {
    text[strcspn(text, "\n")] = 0;

    for (int i = 0; i < count; i++) {
        out[i] = text;
        text = strchr(text, ':');

        if (!text) {
            return i == count - 1;
        }

        if (i == count - 1) {
            return 0;
        }

        *text++ = 0;
    }

    return 1;
}

static int number(const char* text, unsigned* out) {
    char* end;

    if (!*text) {
        return 0;
    }

    *out = (unsigned)strtoul(text, &end, 10);

    return !*end;
}

struct passwd* fgetpwent(FILE* f) {
    static struct passwd entry;
    char* f7[7];

    while (getline(&line, &lineSize, f) >= 0) {
        if (fields(line, f7, 7) && number(f7[2], &entry.pw_uid) && number(f7[3], &entry.pw_gid)) {
            entry.pw_name = f7[0];
            entry.pw_passwd = f7[1];
            entry.pw_gecos = f7[4];
            entry.pw_dir = f7[5];
            entry.pw_shell = f7[6];

            return &entry;
        }
    }

    return 0;
}

int putpwent(const struct passwd* entry, FILE* f) {
    return fprintf(f, "%s:%s:%u:%u:%s:%s:%s\n", entry->pw_name, entry->pw_passwd, entry->pw_uid, entry->pw_gid,
                   entry->pw_gecos, entry->pw_dir, entry->pw_shell) < 0 ? -1 : 0;
}

struct group* fgetgrent(FILE* f) {
    static struct group entry;
    static char** members;
    char* f4[4];

    while (getline(&line, &lineSize, f) >= 0) {
        if (!fields(line, f4, 4) || !number(f4[2], &entry.gr_gid)) {
            continue;
        }

        size_t count = *f4[3] ? 1 : 0;

        for (char* p = f4[3]; *p; p++) {
            count += *p == ',';
        }

        char** grown = realloc(members, (count + 1) * sizeof *members);

        if (!grown) {
            return 0;
        }

        members = grown;
        char* p = f4[3];

        for (size_t i = 0; i < count; i++) {
            members[i] = p;
            p = strchr(p, ',');

            if (p) {
                *p++ = 0;
            }
        }

        members[count] = 0;
        entry.gr_name = f4[0];
        entry.gr_passwd = f4[1];
        entry.gr_mem = members;

        return &entry;
    }

    return 0;
}
