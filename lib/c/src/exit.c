#include <stdlib.h>

void __stdio_exit(void);

/*
 * The handlers exit runs, last registered first: atexit's and C++'s static
 * and thread_local destructors, which __cxa_atexit registers with their
 * object, in one list that grows as needed. Each is called as the type it
 * has: an indirect call of another type traps.
 */
typedef struct {
    void (*plain)(void);
    void (*fn)(void*);
    void* arg;
} Handler;

static Handler* handlers;
static int registered;
static int capacity;

void* __dso_handle;

static int add(Handler h) {
    if (registered == capacity) {
        int grown = capacity ? 2 * capacity : 64;
        Handler* moved = realloc(handlers, grown * sizeof *handlers);

        if (!moved) {
            return -1;
        }

        handlers = moved;
        capacity = grown;
    }

    handlers[registered++] = h;

    return 0;
}

int __cxa_atexit(void (*fn)(void*), void* arg, void* dso) {
    return add((Handler){0, fn, arg});
}

// The one thread's thread_local objects end with it, at exit, as the
// static ones do: the later registered first.
int __cxa_thread_atexit(void (*fn)(void*), void* arg, void* dso) {
    return __cxa_atexit(fn, arg, dso);
}

int atexit(void (*handler)(void)) {
    return add((Handler){handler, 0, 0});
}

void exit(int code) {
    while (registered > 0) {
        Handler h = handlers[--registered];

        if (h.plain) {
            h.plain();
        } else {
            h.fn(h.arg);
        }
    }

    __stdio_exit();
    _Exit(code);
}
