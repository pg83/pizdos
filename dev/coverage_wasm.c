// LLVM's source coverage counters live in the reactor's linear memory.
// Only coverage builds link this shim; it needs no compiler-rt or libc.
// ProfileData follows LLVM 23's llvm/ProfileData/InstrProfData.inc.
#if __clang_major__ != 23
    #error Review the LLVM profile record layout before changing Clang versions.
#endif

typedef __UINTPTR_TYPE__ uptr;

struct ProfileData {
    __UINT64_TYPE__ name;
    __UINT64_TYPE__ hash;
    uptr counters;
    uptr uniformCounters;
    uptr bitmap;
    uptr function;
    uptr values;
    __UINT32_TYPE__ count;
    __UINT16_TYPE__ valueSites[3];
    __UINT16_TYPE__ waveSize;
    __UINT32_TYPE__ bitmapBytes;
};

int __llvm_profile_runtime;
extern unsigned char __start___llvm_prf_data[], __stop___llvm_prf_data[];
extern unsigned char __start___llvm_prf_cnts[], __stop___llvm_prf_cnts[];
extern unsigned char __start___llvm_prf_names[], __stop___llvm_prf_names[];

__attribute__((export_name("__coverage_info"))) uptr coverageInfo(unsigned index) {
    switch (index) {
        case 0: {
            return (uptr)__start___llvm_prf_data;
        }
        case 1: {
            return (uptr)__stop___llvm_prf_data;
        }
        case 2: {
            return (uptr)__start___llvm_prf_cnts;
        }
        case 3: {
            return (uptr)__stop___llvm_prf_cnts;
        }
        case 4: {
            return (uptr)__start___llvm_prf_names;
        }
        case 5: {
            return (uptr)__stop___llvm_prf_names;
        }
        case 6: {
            return __clang_major__;
        }
        case 7: {
            return sizeof(struct ProfileData);
        }
        case 8: {
            return __builtin_offsetof(struct ProfileData, counters);
        }
        case 9: {
            return __builtin_offsetof(struct ProfileData, count);
        }
        case 10: {
            #ifdef AOT_CHAOS
                return 1;
            #else
                return 0;
            #endif
        }
    }

    return 0;
}
