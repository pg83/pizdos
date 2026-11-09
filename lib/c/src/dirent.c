#include <dirent.h>
#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/*
 * scandir calls the program's filter and comparison, so it is the
 * process's: readdir's entries copied, kept by filter, sorted by qsort.
 */

int scandir(const char* path, struct dirent*** list, int (*filter)(const struct dirent*),
            int (*compare)(const struct dirent**, const struct dirent**)) {
    DIR* dir = opendir(path);

    if (!dir) {
        return -1;
    }

    struct dirent** entries = 0;
    size_t count = 0;
    size_t capacity = 0;
    int saved = errno;
    struct dirent* entry;

    errno = 0;

    while ((entry = readdir(dir))) {
        if (filter && !filter(entry)) {
            continue;
        }

        if (count == capacity) {
            capacity = capacity ? 2 * capacity : 16;
            struct dirent** grown = realloc(entries, capacity * sizeof *entries);

            if (!grown) {
                break;
            }

            entries = grown;
        }

        size_t size = offsetof(struct dirent, d_name) + strlen(entry->d_name) + 1;
        struct dirent* copy = malloc(size);

        if (!copy) {
            break;
        }

        memcpy(copy, entry, size);
        entries[count++] = copy;
    }

    closedir(dir);

    if (errno) {
        while (count) {
            free(entries[--count]);
        }

        free(entries);

        return -1;
    }

    errno = saved;

    if (compare) {
        qsort(entries, count, sizeof *entries, (int (*)(const void*, const void*))compare);
    }

    *list = entries;

    return (int)count;
}
