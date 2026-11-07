// Runs a WASI command compiled by wasm2x with prefix "prog".
#include "prog.h"
#include "w2x.h"
#include "wasi.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define UNPAREN(...) __VA_ARGS__

#define MEM(inst) ((w2x_header*)(inst))->mem, ((w2x_header*)(inst))->mem_size

#define ADAPT(name, params, args)                                            \
    uint32_t wasi_snapshot_preview1__##name(void* inst, UNPAREN params) {    \
        return wasi_##name(MEM(inst), UNPAREN args);                         \
    }

WASI_FUNCS(ADAPT)

uint32_t wasi_snapshot_preview1__sched_yield(void* inst) {
    return wasi_sched_yield(MEM(inst));
}

void wasi_snapshot_preview1__proc_exit(void* inst, uint32_t code) {
    wasi_proc_exit(MEM(inst), code);
}

static jmp_buf trap;

void w2x_trap(void* inst, uint32_t code) {
    longjmp(trap, (int)code);
}

// Memory is reserved once at its maximum and grows in place. W2X_MOVE_MEMORY
// moves it to fresh pages on every growth instead, to exercise generated
// code with memory that moves.
static int move_memory;

static uint8_t* reserve(uint64_t size) {
    void* p = mmap(NULL, size ? size : 1, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);

    if (p == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }

    return p;
}

uint32_t w2x_memory_grow(void* inst, uint32_t delta) {
    w2x_header* h = inst;
    uint64_t old = h->mem_size >> 16;

    if (old + delta > PROG_MEMORY_MAX_PAGES) {
        return UINT32_MAX;
    }

    if (delta == 0) {
        return old;
    }

    uint64_t size = (old + delta) << 16;

    if (move_memory) {
        uint8_t* mem = reserve(size);
        memcpy(mem, h->mem, h->mem_size);
        munmap(h->mem, h->mem_size ? h->mem_size : 1);
        w2x_set_memory(inst, mem, size);
    } else {
        w2x_set_memory(inst, h->mem, size);
    }

    return old;
}

uint32_t w2x_table_grow(void* inst, uint32_t table, uint32_t delta, void* init) {
    return UINT32_MAX;
}

static uintptr_t stack_low;

static int run(void* inst) {
    ((w2x_header*)inst)->stack_limit = stack_low + 2 * W2X_STACK_RESERVE;
    int code = setjmp(trap);

    if (code != 0) {
        fprintf(stderr, "wasm trap %d\n", code);
        return 128 + code;
    }

    w2x_init_prog(inst);
    prog__start(inst);
    return 0;
}

int main(int argc, char** argv) {
    wasi_init(argc, argv);
    void* inst = aligned_alloc(64, (PROG_INSTANCE_SIZE + 63) & ~63);
    memset(inst, 0, PROG_INSTANCE_SIZE);
    uint64_t size = (uint64_t)PROG_MEMORY_PAGES << 16;
    move_memory = getenv("W2X_MOVE_MEMORY") != NULL;
    w2x_set_memory(inst, reserve(move_memory ? size : (uint64_t)PROG_MEMORY_MAX_PAGES << 16), size);

#ifdef PROG_TABLE0_SIZE
    w2x_table* table = (w2x_table*)((char*)inst + PROG_TABLE0_OFFSET);
    table->entries = calloc(PROG_TABLE0_SIZE + 1, sizeof(void*));
    table->size = PROG_TABLE0_SIZE;
    table->max = PROG_TABLE0_MAX;
#endif

    return wasi_run(run, inst, &stack_low);
}
