/* test_asan's program: one memory error per case, which the kernel's address
   sanitizer reports under `vm run --asan`, and a clean case, ok, that uses
   the allocator, the string functions, mmap and the file calls as a correct
   program does and prints a line. */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static volatile int sink;

static int ok(void) {
    char* p = malloc(20);
    memset(p, 'a', 20);
    p = realloc(p, 100);
    p[99] = 0;
    sink += memcmp(p, p, 20);
    char* q = malloc(16);
    strcpy(q, "hello");
    strcat(q, " you");
    sink += (int)strlen(q) + strcmp(q, "hello you");
    sink += memchr(q, 'y', 10) != NULL;
    free(q);
    char* big = malloc(100000);
    big[99999] = 1;
    big = realloc(big, 200000);
    big[199999] = 2;
    free(big);
    void* m = mmap(0, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    memset(m, 1, 8192);
    munmap(m, 8192);
    char* a = aligned_alloc(4096, 100);
    a[99] = 1;
    free(a);
    int fd = open("/tmp/asan.txt", O_RDWR | O_CREAT | O_TRUNC, 0644);
    char buf[32] = "data";
    write(fd, buf, 4);
    lseek(fd, 0, SEEK_SET);
    sink += (int)read(fd, buf, 4);
    close(fd);
    free(p);
    printf("ok %d %s\n", sink, buf);
    return 0;
}

static int overflow(void) {
    char* p = malloc(20);
    p[20] = 1;
    sink = p[0];
    return 1;
}

static int after_free(void) {
    char* p = malloc(20);
    p[0] = 1;
    free(p);
    sink = p[0];
    return 1;
}

static int syscall_read(void) {
    char* p = malloc(8);
    free(p);
    int fd = open("/tmp/asan.txt", O_RDWR | O_CREAT, 0644);
    write(fd, "12345678", 8);
    lseek(fd, 0, SEEK_SET);
    sink = (int)read(fd, p, 8);
    return 1;
}

static int string(void) {
    char* p = malloc(4);
    memset(p, 'a', 4);
    sink = (int)strlen(p);
    return 1;
}

static int copy(void) {
    char* s = malloc(16);
    char* d = malloc(32);
    memset(s, 1, 16);
    memcpy(d, s, 32);
    return 1;
}

static int null(void) {
    volatile int* p = (int*)(uintptr_t)atoi("0");
    *p = 1;
    return 1;
}

static int shrink(void) {
    char* p = malloc(32);
    p = realloc(p, 4);
    p[5] = 1;
    return 1;
}

static int mapped(void) {
    char* m = mmap(0, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    munmap(m, 8192);
    m[0] = 1;
    return 1;
}

static int run(void) {
    char* p = malloc(100000);
    p[100000] = 1;
    return 1;
}

static int wide(void) {
    uint32_t* p = malloc(6);
    sink = (int)p[1];
    return 1;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        return 2;
    }
    const struct {
        const char* name;
        int (*run)(void);
    } cases[] = {
        {"ok", ok}, {"overflow", overflow}, {"after_free", after_free}, {"syscall_read", syscall_read},
        {"string", string}, {"copy", copy}, {"null", null}, {"shrink", shrink}, {"mapped", mapped},
        {"run", run}, {"wide", wide},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        if (strcmp(argv[1], cases[i].name) == 0) {
            return cases[i].run();
        }
    }
    return 2;
}
