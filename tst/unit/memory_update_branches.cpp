//@ compile-flags: -g -fno-builtin
//@ stdout: "memory update branches: ok\n"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static volatile unsigned branch;

template<class T, bool zero>
[[gnu::noinline]] static void update(T* pointer, T operand, unsigned operation) {
    T value;
    switch (operation) {
        case 0: value = *pointer + operand; break;
        case 1: value = *pointer - operand; break;
        case 2: value = *pointer & operand; break;
        case 3: value = *pointer | operand; break;
        default: value = *pointer ^ operand; break;
    }
    *pointer = value;
    if (zero ? value == 0 : value != 0) {
        branch = branch + 7;
        branch = branch + 3;
    } else {
        branch = 13;
    }
}

template<class T>
static void check() {
    const T values[] = {0, 1, T(-1), T(T(1) << (sizeof(T) * 8 - 1))};
    const bool comparisons[] = {false, true};
    for (T before : values) {
        for (T operand : values) {
            const T expected[] = {T(before + operand), T(before - operand), T(before & operand), T(before | operand), T(before ^ operand)};
            for (unsigned operation = 0; operation < 5; ++operation) {
                for (bool zero : comparisons) {
                    T value = before;
                    branch = 0;
                    if (zero) {
                        update<T, true>(&value, operand, operation);
                    } else {
                        update<T, false>(&value, operand, operation);
                    }
                    assert(value == expected[operation]);
                    assert(branch == (zero == (value == 0) ? 10 : 13));
                }
            }
        }
    }
}

int main() {
    check<uint32_t>();
    check<uint64_t>();
    puts("memory update branches: ok");
}
