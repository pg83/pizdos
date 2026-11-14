//@ compile-flags: -O2 -D_GNU_SOURCE
//@ stdout: "memory growth returns: ok\n"

// The memory grows while the code is away, and the code accesses the new
// memory where it comes back: from a call of the kernel, at it or through
// a pointer, a longjmp, a signal handler run on the way out of the kernel
// and one run where the code was interrupted, and from memory.grow in a
// callee.

#include <assert.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

// A block larger than the memory the program starts with, never freed:
// every block is new memory.
static const size_t blockSize = 32 << 20;
static char* volatile block;
static jmp_buf grown;
static volatile sig_atomic_t caught;

static size_t memoryBytes() {
#ifdef __wasm__
    return __builtin_wasm_memory_size(0) << 16;
#else
    return 0;
#endif
}

__attribute__((noinline)) static void growBlock() {
    char* p = static_cast<char*>(malloc(blockSize));
    assert(p);
    p[blockSize - 1] = 1;
    block = p;
}

// touch increments the byte at q, past the memory there was before: q is
// an address the code computed, so its check is against the bound, which
// would trap if stale.
__attribute__((noinline)) static void touch(volatile char* q, size_t before) {
    assert(before == 0 || uintptr_t(q) >= before);
    *q = char(*q + 1);
    assert(*q == 2);
}

static void touchBlock(size_t before) {
    touch(block + blockSize - 1, before);
}

__attribute__((noinline)) static void growAndLeave(int depth) {
    if (depth) {
        growAndLeave(depth - 1);
        return;
    }
    growBlock();
    longjmp(grown, 1);
}

static void growInHandler(int sig) {
    assert(sig == SIGUSR1);
    growBlock();
    caught = caught + 1;
}

#ifdef __wasm__
// The address of an import makes the code call it through its adapter.
static void* (*volatile allocate)(size_t) = __kernel_malloc;

__attribute__((noinline)) static uintptr_t growDirectly(int pages) {
    return uintptr_t(__builtin_wasm_memory_grow(0, pages)) << 16;
}
#endif

int main() {
    size_t before = memoryBytes();
    growBlock();
    touchBlock(before);

#ifdef __wasm__
    before = memoryBytes();
    char* p = static_cast<char*>(allocate(blockSize));
    assert(p);
    p[blockSize - 1] = 1;
    touch(p + blockSize - 1, before);
#endif

    before = memoryBytes();
    if (setjmp(grown) == 0) {
        growAndLeave(4);
    }
    touchBlock(before);

    struct sigaction action = {};
    action.sa_handler = growInHandler;
    assert(sigaction(SIGUSR1, &action, nullptr) == 0);
    before = memoryBytes();
    assert(raise(SIGUSR1) == 0);
    assert(caught == 1);
    touchBlock(before);

    before = memoryBytes();
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        _exit(kill(getppid(), SIGUSR1) == 0 ? 0 : 1);
    }
    while (caught != 2) {
    }
    touchBlock(before);
    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

#ifdef __wasm__
    // Last, as the kernel's heap takes the pages above it for its own.
    before = memoryBytes();
    uintptr_t at = growDirectly(int(blockSize >> 16));
    assert(at == before);
    char* end = reinterpret_cast<char*>(at + blockSize - 1);
    *end = 1;
    touch(end, before);
#endif

    printf("memory growth returns: ok\n");
    return 0;
}
