//@ host: false
//@ compile-flags: -g -gdwarf-5 -fno-optimize-sibling-calls
//@ exit-code: 132
//@ stderr-pattern: "(?s)(?=.*unreachable)(?=.*dwarf_5\\.cpp:12)(?=.*dwarf_5\\.cpp:17).*"

#include <stdint.h>

static volatile uint32_t divisor = 0;

[[gnu::noinline]] static uint32_t fault(uint32_t value) {
    if (divisor == 0) {
        __builtin_trap();
    }
    return value / divisor;
}
int main() {
    return int(fault(7));
}
