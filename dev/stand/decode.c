// The work of the stand (dev/stand.py) over files, with one pure module of
// impulse-desktop/decode built natively: its wrapper against the native
// libraries (native.py). It does what `wasm2o stand` does with wasm2o's
// code (bin/wasm2o/stand.go), which says the work and the results: one line
// per file on standard output, the time of the calls on standard error.
//
//     decode KIND FILE...
//
// The module is the one STAND_KIND_IM, _PDF, _DJVU, _SVG or _MAGIC names.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PAGES 16
#define PIXELS 1600

typedef uint64_t fnv;

static double now(void);
static double untimed;

static void fnv_write(fnv* h, const uint8_t* b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        *h = (*h ^ b[i]) * 0x100000001b3ULL;
    }
}

static void fnv_word(fnv* h, uint32_t v) {
    uint8_t b[4] = {v, v >> 8, v >> 16, v >> 24};
    fnv_write(h, b, 4);
}

static uint32_t le32(const uint8_t* b) {
    return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24;
}

#include <setjmp.h>
#include "native.h"

typedef void* ref;
static jmp_buf landing;
char g_exception_cause[256];

void __wasm_call_ctors(void) {
}

void stand_trap(void) {
    longjmp(landing, 1);
}

static uint8_t* at(ref p) {
    return p;
}

static ref put(const void* b, size_t n) {
    ref p = malloc(n ? n : 1);

    if (!p) {
        fprintf(stderr, "malloc of %zu bytes failed\n", n);
        exit(2);
    }

    memcpy(p, b, n);

    return p;
}

static void drop(ref p) {
    free(p);
}

static void fresh(void) {
}

#define CALL(name, ...) name(__VA_ARGS__)

// image adds the image block at p to h and frees it.
static void image(fnv* h, ref p, uint32_t* width, uint32_t* height) {
    uint8_t* b = at(p);
    uint32_t w = le32(b), ht = le32(b + 4);
    double start = now();
    fnv_write(h, b, 8 + (size_t)w * ht * 4);
    untimed += now() - start;
    drop(p);
    *width = w;
    *height = ht;
}

// work leaves the result for one file in out.
static void work(const char* name, const uint8_t* data, size_t len, char* out, size_t size) {
#if defined(STAND_KIND_IM)
    ref p = put(data, len), n = put(name, strlen(name));
    ref r = CALL(decode, p, len, n, strlen(name));
    drop(p);
    drop(n);

    if (!r) {
        snprintf(out, size, "fail");

        return;
    }

    fnv h = 0xcbf29ce484222325ULL;
    uint32_t w, ht;
    image(&h, r, &w, &ht);
    snprintf(out, size, "%ux%u %016llx", w, ht, (unsigned long long)h);
#elif defined(STAND_KIND_PDF) || defined(STAND_KIND_DJVU) || defined(STAND_KIND_SVG)
#if defined(STAND_KIND_PDF)
#define PAGE(f, ...) CALL(pdf_##f, __VA_ARGS__)
#elif defined(STAND_KIND_DJVU)
#define PAGE(f, ...) CALL(djvu_##f, __VA_ARGS__)
#else
#define PAGE(f, ...) CALL(svg_##f, __VA_ARGS__)
#endif
    ref p = put(data, len);
    ref doc = PAGE(open, p, len);

    if (!doc) {
        drop(p);
        snprintf(out, size, "fail");

        return;
    }

    uint32_t pages = PAGE(pages, doc);
    fnv h = 0xcbf29ce484222325ULL;
    fnv geometry = 0xcbf29ce484222325ULL;

    for (uint32_t page = 0; page < pages && page < PAGES; page++) {
        float w = PAGE(width, doc, page), ht = PAGE(height, doc, page);

        if (!(w > 0 && ht > 0)) {
            fnv_word(&h, 0);
            fnv_word(&geometry, 0);

            continue;
        }

        double k = PIXELS / fmax(w, ht);
        double pw = fmax(1, floor((double)w * k + 0.5)), ph = fmax(1, floor((double)ht * k + 0.5));
        ref r = PAGE(render, doc, page, (uint32_t)pw, (uint32_t)ph);

        if (r) {
            uint32_t iw, ih;
            image(&h, r, &iw, &ih);
            fnv_word(&geometry, iw);
            fnv_word(&geometry, ih);
        } else {
            fnv_word(&h, 0);
            fnv_word(&geometry, 0);
        }
    }

    PAGE(close, doc);
    drop(p);
    snprintf(out, size, "%u %016llx %016llx", pages, (unsigned long long)geometry, (unsigned long long)h);
#elif defined(STAND_KIND_MAGIC)
    ref p = put(data, len);
    ref r = (ref)CALL(magic_mime, p, len);
    drop(p);
    snprintf(out, size, "%s", r ? (const char*)at(r) : "fail");
#else
#error "no kind"
#endif
}

static uint8_t* slurp(const char* path, size_t* len) {
    FILE* f = fopen(path, "rb");

    if (!f) {
        perror(path);
        exit(2);
    }

    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* b = malloc(n ? n : 1);

    if (fread(b, 1, n, f) != (size_t)n) {
        perror(path);
        exit(2);
    }

    fclose(f);
    *len = n;

    return b;
}

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);

    return t.tv_sec + t.tv_nsec / 1e9;
}

int main(int argc, char** argv) {
    int first = 1, repeat = 1;

    if (argc > 2 && !strcmp(argv[1], "--repeat")) {
        char* end;
        long n = strtol(argv[2], &end, 10);

        if (*end || n < 1 || n > 10000) {
            fprintf(stderr, "repeat must be in 1..10000\n");

            return 2;
        }

        repeat = (int)n;
        first = 3;
    }

    if (argc <= first + 1) {
        fprintf(stderr, "usage: decode [--repeat N] KIND FILE...\n");

        return 2;
    }

    fresh();
    double total = 0;

    for (int i = first + 1; i < argc; i++) {
        size_t len;
        uint8_t* data = slurp(argv[i], &len);
        const char* name = strrchr(argv[i], '/') ? strrchr(argv[i], '/') + 1 : argv[i];
        static char out[256];
        char want[sizeof(out)];

        for (int run = 0; run < repeat; run++) {
            untimed = 0;
            double start = now();

            if (setjmp(landing) != 0) {
                snprintf(out, sizeof(out), "trap");
            } else {
                work(name, data, len, out, sizeof(out));
            }

            total += now() - start - untimed;

            if (!strcmp(out, "trap")) {
                fresh();
            }

            if (run == 0) {
                memcpy(want, out, sizeof(out));
            } else if (strcmp(want, out)) {
                fprintf(stderr, "%s: result changed on repetition %d\n", name, run + 1);

                return 2;
            }
        }

        printf("%s %s\n", name, out);
        free(data);
    }

    fprintf(stderr, "time %.6f\n", total);

    return 0;
}
