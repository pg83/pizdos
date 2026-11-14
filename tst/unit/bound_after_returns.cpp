//@ host: false
//@ compile-flags: -O2 -D_GNU_SOURCE
//@ stdout: "bound after returns: ok\n"
//@ stderr-pattern: "(?s)(?:.*?out of bounds memory access){7}.*"

// Where the code comes back with the memory grown, an access past the
// memory's accessible end still traps: the bound the code has there is not
// a larger one. Each case runs in a child of fork, which enters by the
// resume stub, and must die of the trap. memory_growth_returns checks the
// other side: no access within the grown memory traps.

#include <assert.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

static const size_t blockSize = 32 << 20;
static jmp_buf grown;
static volatile sig_atomic_t caught;

static size_t memoryBytes() {
    return __builtin_wasm_memory_size(0) << 16;
}

__attribute__((noinline)) static void growBlock() {
    char* p = static_cast<char*>(malloc(blockSize));
    assert(p);
    p[blockSize - 1] = 1;
}

// pokePast writes 4 KiB past the memory's accessible end as it is now.
__attribute__((noinline)) static void poke(volatile char* q) {
    *q = 1;
}

static void pokePast() {
    poke(reinterpret_cast<volatile char*>(memoryBytes() + 4096));
}

static void growInHandler(int sig) {
    assert(sig == SIGUSR1);
    growBlock();
    caught = caught + 1;
}

static void handleUsr1() {
    struct sigaction action = {};
    action.sa_handler = growInHandler;
    assert(sigaction(SIGUSR1, &action, nullptr) == 0);
}

static void* (*volatile allocate)(size_t) = __kernel_malloc;

__attribute__((noinline)) static void growAndLeave(int depth) {
    if (depth) {
        growAndLeave(depth - 1);
        return;
    }
    growBlock();
    longjmp(grown, 1);
}

__attribute__((noinline)) static void growDirectly(int pages) {
    assert(__builtin_wasm_memory_grow(0, pages) != -1);
}

static void afterFork() {
}

static void afterCall() {
    growBlock();
}

static void afterAdapter() {
    char* p = static_cast<char*>(allocate(blockSize));
    assert(p);
}

static void afterLongjmp() {
    if (setjmp(grown) == 0) {
        growAndLeave(4);
    }
}

static void afterRaise() {
    handleUsr1();
    assert(raise(SIGUSR1) == 0);
    assert(caught == 1);
}

static void afterInterrupt() {
    handleUsr1();
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        _exit(kill(getppid(), SIGUSR1) == 0 ? 0 : 1);
    }
    while (caught != 1) {
    }
    int status = 0;
    assert(waitpid(child, &status, 0) == child);
}

static void afterGrow() {
    growDirectly(int(blockSize >> 16));
}

// trapsPast runs path in a child, which must die of the trap of its write
// past the memory after path returns.
static void trapsPast(void (*path)()) {
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        path();
        pokePast();
        _exit(0);
    }
    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);
}

int main() {
    trapsPast(afterFork);
    trapsPast(afterCall);
    trapsPast(afterAdapter);
    trapsPast(afterLongjmp);
    trapsPast(afterRaise);
    trapsPast(afterInterrupt);
    trapsPast(afterGrow);
    printf("bound after returns: ok\n");
    return 0;
}
