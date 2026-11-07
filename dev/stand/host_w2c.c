// Runs a WASI command translated by wasm2c with module name "prog": the
// reference the stand measures wasm2o against.
#include "prog.h"
#include "wasi.h"
#include "wasm-rt-exceptions.h"
#include "wasm-rt-impl.h"

#include <stdio.h>
#include <stdlib.h>

#define UNPAREN(...) __VA_ARGS__

struct w2c_wasi__snapshot__preview1 {
    wasm_rt_memory_t* mem;
};

#define ADAPT(name, params, args)                                                                   \
    uint32_t w2c_wasi__snapshot__preview1_##name(struct w2c_wasi__snapshot__preview1* w, UNPAREN params) { \
        return wasi_##name(w->mem->data, w->mem->size, UNPAREN args);                               \
    }

WASI_FUNCS(ADAPT)

uint32_t w2c_wasi__snapshot__preview1_sched_yield(struct w2c_wasi__snapshot__preview1* w) {
    return wasi_sched_yield(w->mem->data, w->mem->size);
}

void w2c_wasi__snapshot__preview1_proc_exit(struct w2c_wasi__snapshot__preview1* w, uint32_t code) {
    wasi_proc_exit(w->mem->data, w->mem->size, code);
}

static int run(void* arg) {
    wasm_rt_init();
    static w2c_prog inst;
    static struct w2c_wasi__snapshot__preview1 wasi;
    wasm_rt_trap_t code = wasm_rt_impl_try();

    if (code != 0) {
        fprintf(stderr, "wasm trap: %s\n", wasm_rt_strerror(code));
        return 128 + code;
    }

    wasm2c_prog_instantiate(&inst, &wasi);
    wasi.mem = w2c_prog_memory(&inst);
    w2c_prog_0x5Fstart(&inst);
    return 0;
}

int main(int argc, char** argv) {
    wasi_init(argc, argv);
    uintptr_t low;
    return wasi_run(run, NULL, &low);
}
