#include <stdlib.h>

void __stdio_exit(void);

enum {
    maxHandlers = 64,
};

static void (*handlers[maxHandlers])(void);
static int registered;

int atexit(void (*handler)(void)) {
    if (registered == maxHandlers) {
        return -1;
    }

    handlers[registered++] = handler;

    return 0;
}

void exit(int code) {
    while (registered > 0) {
        handlers[--registered]();
    }

    __stdio_exit();
    _Exit(code);
}
