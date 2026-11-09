#include <mntent.h>
#include <stdio.h>
#include <string.h>

/*
 * The mount table's lines, musl's way: blank and comment lines skipped,
 * four fields and the two numbers, which may be missing.
 */

enum {
    lineSize = 4096,
};

FILE* setmntent(const char* path, const char* mode) {
    return fopen(path, mode);
}

int endmntent(FILE* f) {
    if (f) {
        fclose(f);
    }

    return 1;
}

struct mntent* getmntent_r(FILE* f, struct mntent* entry, char* line, int size) {
    int fields[8];

    for (;;) {
        if (!fgets(line, size, f)) {
            return 0;
        }

        size_t length = strlen(line);

        if (length && line[length - 1] != '\n' && !feof(f)) {
            int c;

            while ((c = getc(f)) != EOF && c != '\n') {
            }

            continue;
        }

        for (int i = 0; i < 8; i++) {
            fields[i] = (int)length;
        }

        entry->mnt_freq = 0;
        entry->mnt_passno = 0;
        sscanf(line, " %n%*[^ \t]%n %n%*[^ \t]%n %n%*[^ \t]%n %n%*[^ \t]%n %d %d", &fields[0], &fields[1], &fields[2], &fields[3],
               &fields[4], &fields[5], &fields[6], &fields[7], &entry->mnt_freq, &entry->mnt_passno);

        if (line[fields[0]] == '#' || fields[1] == (int)length) {
            continue;
        }

        line[fields[1]] = 0;
        line[fields[3]] = 0;
        line[fields[5]] = 0;
        line[fields[7]] = 0;
        entry->mnt_fsname = line + fields[0];
        entry->mnt_dir = line + fields[2];
        entry->mnt_type = line + fields[4];
        entry->mnt_opts = line + fields[6];

        return entry;
    }
}

struct mntent* getmntent(FILE* f) {
    static char line[lineSize];
    static struct mntent entry;

    return getmntent_r(f, &entry, line, sizeof line);
}

int addmntent(FILE* f, const struct mntent* entry) {
    if (fseek(f, 0, SEEK_END)) {
        return 1;
    }

    return fprintf(f, "%s\t%s\t%s\t%s\t%d\t%d\n", entry->mnt_fsname, entry->mnt_dir, entry->mnt_type, entry->mnt_opts,
                   entry->mnt_freq, entry->mnt_passno) < 0;
}

char* hasmntopt(const struct mntent* entry, const char* option) {
    return strstr(entry->mnt_opts, option);
}
