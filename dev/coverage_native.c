/* Own the flush for normal exit, _exit and abort. A libc exit path can
   reach both atexit and _exit; merging the same counters twice compounds
   earlier runs in LLVM's profile pool until they overflow. */
int __llvm_profile_runtime;
void __llvm_profile_initialize_file(void);
int __llvm_profile_dump(void);
int atexit(void (*callback)(void));
void __real__exit(int status) __attribute__((noreturn));
void __real_abort(void) __attribute__((noreturn));

static int written;

static void writeOnce(void) {
    if (!written) {
        written = 1;
        __llvm_profile_dump();
    }
}

__attribute__((constructor)) static void initialize(void) {
    __llvm_profile_initialize_file();
    atexit(writeOnce);
}

void __wrap__exit(int status) {
    writeOnce();
    __real__exit(status);
}

void __wrap_abort(void) {
    writeOnce();
    __real_abort();
}
