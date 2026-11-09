/*
 * A program whose main takes no arguments: clang names it __main_void, and
 * the crt calls __main_argc_argv. The archive brings this object in only
 * when the program has not defined __main_argc_argv itself.
 */
int __main_void(void);

int __main_argc_argv(int argc, char** argv) {
    (void)argc;
    (void)argv;

    return __main_void();
}
