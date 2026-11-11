//@ compile-flags: -g -D_GNU_SOURCE
//@ stdout: "random state: ok\n"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

int main() {
    int integers[128];
    srand(123);
    for (int& value : integers) {
        value = rand();
        assert(value >= 0 && value <= RAND_MAX);
    }
    srand(123);
    for (int value : integers) {
        assert(rand() == value);
    }
    unsigned firstSeed = 17, secondSeed = 17;
    for (int i = 0; i < 100; ++i) {
        int value = rand_r(&firstSeed);
        assert(value >= 0 && value == rand_r(&secondSeed) && firstSeed == secondSeed);
    }
    long sequence[128];
    srandom(321);
    for (long& value : sequence) {
        value = random();
        assert(value >= 0 && value <= 0x7fffffff);
    }
    srandom(321);
    for (long value : sequence) {
        assert(random() == value);
    }
    alignas(long) char state[512];
    const size_t sizes[] = {8, 16, 32, 64, 128, 256, 512};
    for (size_t size : sizes) {
        char* previous = initstate(42, state, size);
        assert(previous != nullptr);
        for (long& value : sequence) {
            value = random();
            assert(value >= 0 && value <= 0x7fffffff);
        }
        assert(initstate(42, state, size) == state);
        for (long value : sequence) {
            assert(random() == value);
        }
        assert(setstate(previous) == state);
    }
    alignas(long) char stateA[128], stateB[256];
    char* original = initstate(12, stateA, sizeof(stateA));
    assert(original);
    for (int i = 0; i < 10; ++i) {
        random();
    }
    assert(initstate(34, stateB, sizeof(stateB)) == stateA);
    for (int i = 0; i < 10; ++i) {
        random();
    }
    assert(setstate(stateA) == stateB);
    long continuedA = random();
    assert(setstate(stateB) == stateA);
    long continuedB = random();
    assert(initstate(12, stateA, sizeof(stateA)) == stateB);
    for (int i = 0; i < 10; ++i) {
        random();
    }
    assert(random() == continuedA);
    assert(initstate(34, stateB, sizeof(stateB)) == stateA);
    for (int i = 0; i < 10; ++i) {
        random();
    }
    assert(random() == continuedB);
    assert(setstate(original) == stateB);
    assert(initstate(0, state, 7) == nullptr);
    puts("random state: ok");
}
