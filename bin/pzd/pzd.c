/*
 * pzd: a module packed into an executable of the kernel, as dev/exe.py
 * packs one: "PZD1" and the executable's length, header included, a 32-bit
 * little-endian number; the module as one zstd frame (RFC 8878) of raw
 * blocks, which needs no compressor; then the tail, when given, the
 * program's own, which the kernel never reads.
 *
 *     pzd <module.wasm> <executable> [<tail>]
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* A raw block holds at most 128 KiB, the format's Block_Maximum_Size. */
#define BLOCK_MAX (128 * 1024)

static unsigned char* readAll(const char* path, size_t* size) {
    FILE* f = fopen(path, "rb");
    struct stat st;

    if (f == NULL || fstat(fileno(f), &st) != 0) {
        fprintf(stderr, "pzd: %s: %s\n", path, strerror(errno));
        exit(1);
    }

    unsigned char* data = malloc(st.st_size ? (size_t)st.st_size : 1);

    if (data == NULL || fread(data, 1, (size_t)st.st_size, f) != (size_t)st.st_size) {
        fprintf(stderr, "pzd: %s: cannot read it\n", path);
        exit(1);
    }

    fclose(f);
    *size = (size_t)st.st_size;

    return data;
}

static void put(FILE* out, const void* data, size_t size, const char* path) {
    if (fwrite(data, 1, size, out) != size) {
        fprintf(stderr, "pzd: %s: %s\n", path, strerror(errno));
        exit(1);
    }
}

static void put32(unsigned char* at, uint32_t value) {
    at[0] = (unsigned char)value;
    at[1] = (unsigned char)(value >> 8);
    at[2] = (unsigned char)(value >> 16);
    at[3] = (unsigned char)(value >> 24);
}

int main(int argc, char** argv) {
    if (argc != 3 && argc != 4) {
        fprintf(stderr, "usage: pzd <module.wasm> <executable> [<tail>]\n");
        return 2;
    }

    size_t size, tailSize = 0;
    unsigned char* module = readAll(argv[1], &size);
    unsigned char* tail = argc == 4 ? readAll(argv[3], &tailSize) : NULL;

    if (size < 8 || memcmp(module, "\0asm\1\0\0\0", 8) != 0) {
        fprintf(stderr, "pzd: %s: not a WebAssembly module\n", argv[1]);
        return 1;
    }

    uint64_t blocks = (size + BLOCK_MAX - 1) / BLOCK_MAX;
    uint64_t length = 8 + 4 + 1 + 4 + 3 * blocks + size;

    if (length > UINT32_MAX) {
        fprintf(stderr, "pzd: %s: an executable past 4 GiB\n", argv[1]);
        return 1;
    }

    FILE* out = fopen(argv[2], "wb");

    if (out == NULL) {
        fprintf(stderr, "pzd: %s: %s\n", argv[2], strerror(errno));
        return 1;
    }

    /* The header; the frame's magic; its descriptor: a 4-byte content
     * size, a single segment, so the window is the content, and no
     * checksum; the content's size. */
    unsigned char head[17] = {'P', 'Z', 'D', '1', 0, 0, 0, 0, 0x28, 0xb5, 0x2f, 0xfd, 0xa0};
    put32(head + 4, (uint32_t)length);
    put32(head + 13, (uint32_t)size);
    put(out, head, sizeof head, argv[2]);

    /* Raw blocks: each a 3-byte header, the last block's bit, the type 0
     * and the size, then the bytes. */
    for (size_t at = 0; at < size;) {
        size_t n = size - at < BLOCK_MAX ? size - at : BLOCK_MAX;
        uint32_t block = (at + n == size ? 1u : 0u) | (uint32_t)n << 3;
        unsigned char header[3] = {(unsigned char)block, (unsigned char)(block >> 8), (unsigned char)(block >> 16)};
        put(out, header, sizeof header, argv[2]);
        put(out, module + at, n, argv[2]);
        at += n;
    }

    put(out, tail, tailSize, argv[2]);

    if (fclose(out) != 0 || chmod(argv[2], 0755) != 0) {
        fprintf(stderr, "pzd: %s: %s\n", argv[2], strerror(errno));
        return 1;
    }

    return 0;
}
