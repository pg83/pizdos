/* Coverage builds only: compiler diagnostics bypass atexit handlers. */
int __llvm_profile_write_file(void);
void __real__exit(int status) __attribute__((noreturn));
void __real_abort(void) __attribute__((noreturn));

void __wrap__exit(int status) {
    __llvm_profile_write_file();
    __real__exit(status);
}

void __wrap_abort(void) {
    __llvm_profile_write_file();
    __real_abort();
}
