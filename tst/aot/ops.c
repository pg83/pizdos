// What test_aot.py runs natively after aot: every operator class clang emits
// for our programs, one exported function per case, inputs from the caller so
// nothing folds at compile time.

#define EXPORT(name) __attribute__((export_name(#name)))
#define KERNEL(name) __attribute__((import_module("kernel"), import_name(#name)))

KERNEL(add) int kernelAdd(int a, int b);

#define BINARY(name, type, op) \
    EXPORT(name) type name(type a, type b) { \
        return a op b; \
    }

BINARY(i32_add, int, +)
BINARY(i32_sub, int, -)
BINARY(i32_mul, int, *)
BINARY(i32_div_s, int, /)
BINARY(i32_rem_s, int, %)
BINARY(i32_div_u, unsigned, /)
BINARY(i32_rem_u, unsigned, %)
BINARY(i32_and, int, &)
BINARY(i32_or, int, |)
BINARY(i32_xor, int, ^)
BINARY(i64_add, long long, +)
BINARY(i64_sub, long long, -)
BINARY(i64_mul, long long, *)
BINARY(i64_div_s, long long, /)
BINARY(i64_rem_s, long long, %)
BINARY(i64_div_u, unsigned long long, /)
BINARY(i64_rem_u, unsigned long long, %)
BINARY(i64_and, long long, &)
BINARY(i64_or, long long, |)
BINARY(i64_xor, long long, ^)
BINARY(f32_add, float, +)
BINARY(f32_sub, float, -)
BINARY(f32_mul, float, *)
BINARY(f32_div, float, /)
BINARY(f64_add, double, +)
BINARY(f64_sub, double, -)
BINARY(f64_mul, double, *)
BINARY(f64_div, double, /)

EXPORT(i32_shl) int i32_shl(int a, int b) {
    return a << (b & 31);
}

EXPORT(i32_shr_s) int i32_shr_s(int a, int b) {
    return a >> (b & 31);
}

EXPORT(i32_shr_u) unsigned i32_shr_u(unsigned a, int b) {
    return a >> (b & 31);
}

EXPORT(i32_rotl) unsigned i32_rotl(unsigned a, unsigned b) {
    return __builtin_rotateleft32(a, b);
}

EXPORT(i32_rotr) unsigned i32_rotr(unsigned a, unsigned b) {
    return __builtin_rotateright32(a, b);
}

EXPORT(i64_shl) long long i64_shl(long long a, long long b) {
    return a << (b & 63);
}

EXPORT(i64_shr_s) long long i64_shr_s(long long a, long long b) {
    return a >> (b & 63);
}

EXPORT(i64_shr_u) unsigned long long i64_shr_u(unsigned long long a, long long b) {
    return a >> (b & 63);
}

EXPORT(i64_rotl) unsigned long long i64_rotl(unsigned long long a, unsigned long long b) {
    return __builtin_rotateleft64(a, b);
}

EXPORT(i64_rotr) unsigned long long i64_rotr(unsigned long long a, unsigned long long b) {
    return __builtin_rotateright64(a, b);
}

EXPORT(i32_clz) int i32_clz(unsigned a) {
    return a ? __builtin_clz(a) : 32;
}

EXPORT(i32_ctz) int i32_ctz(unsigned a) {
    return a ? __builtin_ctz(a) : 32;
}

EXPORT(i32_popcnt) int i32_popcnt(unsigned a) {
    return __builtin_popcount(a);
}

EXPORT(i64_clz) long long i64_clz(unsigned long long a) {
    return a ? __builtin_clzll(a) : 64;
}

EXPORT(i64_ctz) long long i64_ctz(unsigned long long a) {
    return a ? __builtin_ctzll(a) : 64;
}

EXPORT(i64_popcnt) long long i64_popcnt(unsigned long long a) {
    return __builtin_popcountll(a);
}

// Every comparison at once, one bit each.
EXPORT(i32_compare) int i32_compare(int a, int b) {
    unsigned x = a;
    unsigned y = b;

    return (a == b) | (a != b) << 1 | (a < b) << 2 | (x < y) << 3 | (a > b) << 4 | (x > y) << 5 | (a <= b) << 6 | (x <= y) << 7 | (a >= b) << 8 | (x >= y) << 9 | (!a) << 10;
}

EXPORT(i64_compare) int i64_compare(long long a, long long b) {
    unsigned long long x = a;
    unsigned long long y = b;

    return (a == b) | (a != b) << 1 | (a < b) << 2 | (x < y) << 3 | (a > b) << 4 | (x > y) << 5 | (a <= b) << 6 | (x <= y) << 7 | (a >= b) << 8 | (x >= y) << 9 | (!a) << 10;
}

EXPORT(f32_compare) int f32_compare(float a, float b) {
    return (a == b) | (a != b) << 1 | (a < b) << 2 | (a > b) << 3 | (a <= b) << 4 | (a >= b) << 5;
}

EXPORT(f64_compare) int f64_compare(double a, double b) {
    return (a == b) | (a != b) << 1 | (a < b) << 2 | (a > b) << 3 | (a <= b) << 4 | (a >= b) << 5;
}

#define FLOAT_UNARY(name, type, expression) \
    EXPORT(name) type name(type a) { \
        return expression; \
    }

FLOAT_UNARY(f32_abs, float, __builtin_fabsf(a))
FLOAT_UNARY(f32_neg, float, -a)
FLOAT_UNARY(f32_sqrt, float, __builtin_sqrtf(a))
FLOAT_UNARY(f32_ceil, float, __builtin_ceilf(a))
FLOAT_UNARY(f32_floor, float, __builtin_floorf(a))
FLOAT_UNARY(f32_trunc, float, __builtin_truncf(a))
FLOAT_UNARY(f32_nearest, float, __builtin_roundevenf(a))
FLOAT_UNARY(f64_abs, double, __builtin_fabs(a))
FLOAT_UNARY(f64_neg, double, -a)
FLOAT_UNARY(f64_sqrt, double, __builtin_sqrt(a))
FLOAT_UNARY(f64_ceil, double, __builtin_ceil(a))
FLOAT_UNARY(f64_floor, double, __builtin_floor(a))
FLOAT_UNARY(f64_trunc, double, __builtin_trunc(a))
FLOAT_UNARY(f64_nearest, double, __builtin_roundeven(a))

EXPORT(f32_min) float f32_min(float a, float b) {
    return __builtin_wasm_min_f32(a, b);
}

EXPORT(f32_max) float f32_max(float a, float b) {
    return __builtin_wasm_max_f32(a, b);
}

EXPORT(f64_min) double f64_min(double a, double b) {
    return __builtin_wasm_min_f64(a, b);
}

EXPORT(f64_max) double f64_max(double a, double b) {
    return __builtin_wasm_max_f64(a, b);
}

EXPORT(f32_copysign) float f32_copysign(float a, float b) {
    return __builtin_copysignf(a, b);
}

EXPORT(f64_copysign) double f64_copysign(double a, double b) {
    return __builtin_copysign(a, b);
}

#define CONVERT(name, from, to, expression) \
    EXPORT(name) to name(from a) { \
        return expression; \
    }

CONVERT(i32_trunc_f32_s, float, int, __builtin_wasm_trunc_s_i32_f32(a))
CONVERT(i32_trunc_f32_u, float, unsigned, __builtin_wasm_trunc_u_i32_f32(a))
CONVERT(i32_trunc_f64_s, double, int, __builtin_wasm_trunc_s_i32_f64(a))
CONVERT(i32_trunc_f64_u, double, unsigned, __builtin_wasm_trunc_u_i32_f64(a))
CONVERT(i64_trunc_f32_s, float, long long, __builtin_wasm_trunc_s_i64_f32(a))
CONVERT(i64_trunc_f32_u, float, unsigned long long, __builtin_wasm_trunc_u_i64_f32(a))
CONVERT(i64_trunc_f64_s, double, long long, __builtin_wasm_trunc_s_i64_f64(a))
CONVERT(i64_trunc_f64_u, double, unsigned long long, __builtin_wasm_trunc_u_i64_f64(a))
CONVERT(i32_trunc_sat_f32_s, float, int, __builtin_wasm_trunc_saturate_s_i32_f32(a))
CONVERT(i32_trunc_sat_f32_u, float, unsigned, __builtin_wasm_trunc_saturate_u_i32_f32(a))
CONVERT(i32_trunc_sat_f64_s, double, int, __builtin_wasm_trunc_saturate_s_i32_f64(a))
CONVERT(i32_trunc_sat_f64_u, double, unsigned, __builtin_wasm_trunc_saturate_u_i32_f64(a))
CONVERT(i64_trunc_sat_f32_s, float, long long, __builtin_wasm_trunc_saturate_s_i64_f32(a))
CONVERT(i64_trunc_sat_f32_u, float, unsigned long long, __builtin_wasm_trunc_saturate_u_i64_f32(a))
CONVERT(i64_trunc_sat_f64_s, double, long long, __builtin_wasm_trunc_saturate_s_i64_f64(a))
CONVERT(i64_trunc_sat_f64_u, double, unsigned long long, __builtin_wasm_trunc_saturate_u_i64_f64(a))
CONVERT(f32_convert_i32_s, int, float, (float)a)
CONVERT(f32_convert_i32_u, unsigned, float, (float)a)
CONVERT(f32_convert_i64_s, long long, float, (float)a)
CONVERT(f32_convert_i64_u, unsigned long long, float, (float)a)
CONVERT(f64_convert_i32_s, int, double, (double)a)
CONVERT(f64_convert_i32_u, unsigned, double, (double)a)
CONVERT(f64_convert_i64_s, long long, double, (double)a)
CONVERT(f64_convert_i64_u, unsigned long long, double, (double)a)
CONVERT(f32_demote_f64, double, float, (float)a)
CONVERT(f64_promote_f32, float, double, (double)a)
CONVERT(i32_reinterpret_f32, float, int, __builtin_bit_cast(int, a))
CONVERT(i64_reinterpret_f64, double, long long, __builtin_bit_cast(long long, a))
CONVERT(f32_reinterpret_i32, int, float, __builtin_bit_cast(float, a))
CONVERT(f64_reinterpret_i64, long long, double, __builtin_bit_cast(double, a))
CONVERT(i32_extend8_s, int, int, (signed char)a)
CONVERT(i32_extend16_s, int, int, (short)a)
CONVERT(i64_extend8_s, long long, long long, (signed char)a)
CONVERT(i64_extend16_s, long long, long long, (short)a)
CONVERT(i64_extend32_s, long long, long long, (int)a)
CONVERT(i64_extend_i32_s, int, long long, a)
CONVERT(i64_extend_i32_u, unsigned, long long, a)
CONVERT(i32_wrap_i64, long long, int, (int)a)

// Memory at an address the caller picks, every access width.
#define PEEK(name, type) \
    EXPORT(name) long long name(int address) { \
        return *(volatile type*)address; \
    }

#define POKE(name, type) \
    EXPORT(name) void name(int address, long long value) { \
        *(volatile type*)address = value; \
    }

PEEK(peek8s, signed char)
PEEK(peek8u, unsigned char)
PEEK(peek16s, short)
PEEK(peek16u, unsigned short)
PEEK(peek32s, int)
PEEK(peek32u, unsigned)
PEEK(peek64, long long)
POKE(poke8, char)
POKE(poke16, short)
POKE(poke32, int)
POKE(poke64, long long)

EXPORT(peekf32) float peekf32(int address) {
    return *(volatile float*)address;
}

EXPORT(peekf64) double peekf64(int address) {
    return *(volatile double*)address;
}

EXPORT(pokef32) void pokef32(int address, float value) {
    *(volatile float*)address = value;
}

EXPORT(pokef64) void pokef64(int address, double value) {
    *(volatile double*)address = value;
}

EXPORT(copy) void copy(char* to, const char* from, unsigned size) {
    __builtin_memcpy(to, from, size);
}

EXPORT(fill) void fill(char* to, int byte, unsigned size) {
    __builtin_memset(to, byte, size);
}

EXPORT(memory_grow) int memory_grow(int pages) {
    return __builtin_wasm_memory_grow(0, pages);
}

EXPORT(memory_size) int memory_size(void) {
    return __builtin_wasm_memory_size(0);
}

static char message[] = "data segment";

EXPORT(message_address) int message_address(void) {
    return (int)message;
}

// Control flow: recursion, loops, a dense switch, selects.
EXPORT(fib) int fib(int n) {
    return n < 2 ? n : fib(n - 1) + fib(n - 2);
}

EXPORT(collatz) int collatz(long long n) {
    int steps = 0;

    while (n != 1) {
        n = n & 1 ? 3 * n + 1 : n / 2;
        steps++;
    }

    return steps;
}

EXPORT(classify) int classify(int x) {
    switch (x) {
        case 0:
            return 17;
        case 1:
            return 4;
        case 2:
            return 99;
        case 3:
            return -5;
        case 4:
            return 1000;
        case 5:
            return 3;
        case 7:
            return 70;
        case 8:
            return 81;
        default:
            return -1;
    }
}

EXPORT(max_s) int max_s(int a, int b) {
    return a > b ? a : b;
}

// A frame on the shadow stack: the array's address escapes, so it lives in
// linear memory below __stack_pointer.
EXPORT(stack_sum) int stack_sum(int n) {
    volatile int values[64];
    int sum = 0;

    for (int i = 0; i < 64; i++) {
        values[i] = i * n;
    }

    for (int i = 0; i < 64; i++) {
        sum += values[i];
    }

    return sum;
}

EXPORT(spin) int spin(int n) {
    volatile int count = 0;

    for (int i = 0; i < n; i++) {
        count++;
    }

    return count;
}

// Function pointers are table indices; the table holds a kernel import too.
static int negate(int a) {
    return -a;
}

static int twice(int a) {
    return a * 2;
}

static int sum(int a, int b) {
    return a + b;
}

static int product(int a, int b) {
    return a * b;
}

static int (*binaries[])(int, int) = {sum, product, kernelAdd};

EXPORT(dispatch) int dispatch(int which, int a, int b) {
    return binaries[which](a, b);
}

EXPORT(unary_pointer) int unary_pointer(int which) {
    return which ? (int)twice : (int)negate;
}

EXPORT(binary_pointer) int binary_pointer(void) {
    return (int)sum;
}

EXPORT(call_unary) int call_unary(int pointer, int a) {
    return ((int (*)(int))pointer)(a);
}

EXPORT(call_kernel) int call_kernel(int a, int b) {
    return kernelAdd(a, b) * 3;
}

EXPORT(unreachable) void unreachable(void) {
    __builtin_trap();
}
