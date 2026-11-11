//@ host: false
//@ compile-flags: -g -gdwarf-5 -fno-optimize-sibling-calls
//@ exit-code: 139
//@ stderr-pattern: "(?s)(?=.*out of bounds memory access)(?=.*dwarf_5_bounds\\.cpp:102)(?=.*dwarf_5_bounds\\.cpp:201).*"

#include <stdint.h>

#line 100
[[gnu::noinline]] static uint32_t readAt(uint32_t index) {
    auto* address = reinterpret_cast<volatile uint32_t*>(uintptr_t(index));
    uint32_t value = *address;
    return value + 1;
}
#line 200
int main() {
    return int(readAt(uint32_t((__builtin_wasm_memory_size(0) + 1) * 65536)));
}
