// The contract between wasm2o-compiled code and its host.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// An instance is <PREFIX>_INSTANCE_SIZE bytes the host allocates zeroed,
// 16-byte aligned. It starts with this header; the host fills it, then
// calls <prefix>_init.
typedef struct w2x_header {
    uint8_t* mem;          // base of linear memory
    uint64_t mem_size;     // its size in bytes
    int64_t mem_bound;     // mem_size - W2X_BOUND_SLACK: see w2x_set_memory
    uintptr_t stack_limit; // generated code traps when the stack reaches it
    void* host;            // free for the host
    uint64_t reserved;
} w2x_header;

// Generated code checks an address against mem_bound and lets accesses
// reach up to this far past it.
#define W2X_BOUND_SLACK 65536

// Leaf functions with small frames do not check the stack: the host must
// leave at least this much stack below stack_limit.
#define W2X_STACK_RESERVE 65536

// A table is an array of function references: a reference is the address of
// a descriptor in the instance, null for ref.null.
typedef struct w2x_table {
    void** entries;
    uint32_t size;
    uint32_t max;
} w2x_table;

typedef struct w2x_func {
    void* code;
    uint64_t type;
} w2x_func;

enum {
    W2X_TRAP_UNREACHABLE = 1,
    W2X_TRAP_MEMORY_OOB = 2,
    W2X_TRAP_INT_OVERFLOW = 3,
    W2X_TRAP_DIV_BY_ZERO = 4,
    W2X_TRAP_CONVERSION = 5,
    W2X_TRAP_INDIRECT_TYPE = 6,
    W2X_TRAP_UNDEFINED_ELEMENT = 7,
    W2X_TRAP_UNINITIALIZED_ELEMENT = 8,
    W2X_TRAP_STACK = 9,
    W2X_TRAP_TABLE_OOB = 10,
    W2X_TRAP_NULL = 11,
};

// Memory changes go through this: it keeps mem_bound consistent.
static inline void w2x_set_memory(void* instance, uint8_t* mem, uint64_t size) {
    w2x_header* h = (w2x_header*)instance;
    h->mem = mem;
    h->mem_size = size;
    h->mem_bound = (int64_t)size - W2X_BOUND_SLACK;
}

// Provided by the host. w2x_trap must not return: it unwinds to the host,
// with longjmp for example. w2x_memory_grow grows memory by delta pages,
// updating it through w2x_set_memory, and returns the old size in pages or
// UINT32_MAX. Generated code calls memmove and memset as well.
void w2x_trap(void* instance, uint32_t code) __attribute__((noreturn));
uint32_t w2x_memory_grow(void* instance, uint32_t delta);
uint32_t w2x_table_grow(void* instance, uint32_t table, uint32_t delta, void* init);

#ifdef __cplusplus
}
#endif
