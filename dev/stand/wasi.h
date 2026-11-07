// WASI preview1 for the stand's host of wasm2c's code (host_w2c.c). Every
// call takes the base and size of the guest memory, which the host adapts
// wasm2c's calling convention to.
#pragma once

#include <stdint.h>

void wasi_init(int argc, char** argv);

uint32_t wasi_args_get(uint8_t* m, uint64_t n, uint32_t argv, uint32_t buf);
uint32_t wasi_args_sizes_get(uint8_t* m, uint64_t n, uint32_t argc, uint32_t size);
uint32_t wasi_environ_get(uint8_t* m, uint64_t n, uint32_t env, uint32_t buf);
uint32_t wasi_environ_sizes_get(uint8_t* m, uint64_t n, uint32_t count, uint32_t size);
uint32_t wasi_clock_time_get(uint8_t* m, uint64_t n, uint32_t id, uint64_t precision, uint32_t time);
uint32_t wasi_clock_res_get(uint8_t* m, uint64_t n, uint32_t id, uint32_t res);
uint32_t wasi_fd_write(uint8_t* m, uint64_t n, uint32_t fd, uint32_t iovs, uint32_t count, uint32_t written);
uint32_t wasi_fd_read(uint8_t* m, uint64_t n, uint32_t fd, uint32_t iovs, uint32_t count, uint32_t read);
uint32_t wasi_fd_pwrite(uint8_t* m, uint64_t n, uint32_t fd, uint32_t iovs, uint32_t count, uint64_t offset, uint32_t written);
uint32_t wasi_fd_pread(uint8_t* m, uint64_t n, uint32_t fd, uint32_t iovs, uint32_t count, uint64_t offset, uint32_t read);
uint32_t wasi_fd_close(uint8_t* m, uint64_t n, uint32_t fd);
uint32_t wasi_fd_seek(uint8_t* m, uint64_t n, uint32_t fd, uint64_t offset, uint32_t whence, uint32_t result);
uint32_t wasi_fd_sync(uint8_t* m, uint64_t n, uint32_t fd);
uint32_t wasi_fd_fdstat_get(uint8_t* m, uint64_t n, uint32_t fd, uint32_t stat);
uint32_t wasi_fd_fdstat_set_flags(uint8_t* m, uint64_t n, uint32_t fd, uint32_t flags);
uint32_t wasi_fd_filestat_get(uint8_t* m, uint64_t n, uint32_t fd, uint32_t stat);
uint32_t wasi_fd_filestat_set_size(uint8_t* m, uint64_t n, uint32_t fd, uint64_t size);
uint32_t wasi_fd_prestat_get(uint8_t* m, uint64_t n, uint32_t fd, uint32_t stat);
uint32_t wasi_fd_prestat_dir_name(uint8_t* m, uint64_t n, uint32_t fd, uint32_t path, uint32_t len);
uint32_t wasi_fd_readdir(uint8_t* m, uint64_t n, uint32_t fd, uint32_t buf, uint32_t len, uint64_t cookie, uint32_t used);
uint32_t wasi_path_filestat_get(uint8_t* m, uint64_t n, uint32_t fd, uint32_t flags, uint32_t path, uint32_t len, uint32_t stat);
uint32_t wasi_path_open(uint8_t* m, uint64_t n, uint32_t fd, uint32_t dirflags, uint32_t path, uint32_t len, uint32_t oflags,
                        uint64_t rights, uint64_t inheriting, uint32_t fdflags, uint32_t opened);
uint32_t wasi_path_create_directory(uint8_t* m, uint64_t n, uint32_t fd, uint32_t path, uint32_t len);
uint32_t wasi_path_remove_directory(uint8_t* m, uint64_t n, uint32_t fd, uint32_t path, uint32_t len);
uint32_t wasi_path_unlink_file(uint8_t* m, uint64_t n, uint32_t fd, uint32_t path, uint32_t len);
uint32_t wasi_path_rename(uint8_t* m, uint64_t n, uint32_t fd, uint32_t path, uint32_t len, uint32_t fd2, uint32_t path2, uint32_t len2);
uint32_t wasi_path_readlink(uint8_t* m, uint64_t n, uint32_t fd, uint32_t path, uint32_t len, uint32_t buf, uint32_t buflen, uint32_t used);
uint32_t wasi_poll_oneoff(uint8_t* m, uint64_t n, uint32_t in, uint32_t out, uint32_t count, uint32_t events);
uint32_t wasi_random_get(uint8_t* m, uint64_t n, uint32_t buf, uint32_t len);
uint32_t wasi_sched_yield(uint8_t* m, uint64_t n);
void wasi_proc_exit(uint8_t* m, uint64_t n, uint32_t code) __attribute__((noreturn));

// runs fn(arg) on a thread with a large stack and returns its result
int wasi_run(int (*fn)(void*), void* arg, uintptr_t* stack_low);

// WASI_FUNCS(X) lists the functions with arguments as X(name, (params),
// (args)) for the adapters; sched_yield and proc_exit are adapted by hand.
#define WASI_FUNCS(X) \
    X(args_get, (uint32_t a, uint32_t b), (a, b)) \
    X(args_sizes_get, (uint32_t a, uint32_t b), (a, b)) \
    X(environ_get, (uint32_t a, uint32_t b), (a, b)) \
    X(environ_sizes_get, (uint32_t a, uint32_t b), (a, b)) \
    X(clock_time_get, (uint32_t a, uint64_t b, uint32_t c), (a, b, c)) \
    X(clock_res_get, (uint32_t a, uint32_t b), (a, b)) \
    X(fd_write, (uint32_t a, uint32_t b, uint32_t c, uint32_t d), (a, b, c, d)) \
    X(fd_read, (uint32_t a, uint32_t b, uint32_t c, uint32_t d), (a, b, c, d)) \
    X(fd_pwrite, (uint32_t a, uint32_t b, uint32_t c, uint64_t d, uint32_t e), (a, b, c, d, e)) \
    X(fd_pread, (uint32_t a, uint32_t b, uint32_t c, uint64_t d, uint32_t e), (a, b, c, d, e)) \
    X(fd_close, (uint32_t a), (a)) \
    X(fd_seek, (uint32_t a, uint64_t b, uint32_t c, uint32_t d), (a, b, c, d)) \
    X(fd_sync, (uint32_t a), (a)) \
    X(fd_fdstat_get, (uint32_t a, uint32_t b), (a, b)) \
    X(fd_fdstat_set_flags, (uint32_t a, uint32_t b), (a, b)) \
    X(fd_filestat_get, (uint32_t a, uint32_t b), (a, b)) \
    X(fd_filestat_set_size, (uint32_t a, uint64_t b), (a, b)) \
    X(fd_prestat_get, (uint32_t a, uint32_t b), (a, b)) \
    X(fd_prestat_dir_name, (uint32_t a, uint32_t b, uint32_t c), (a, b, c)) \
    X(fd_readdir, (uint32_t a, uint32_t b, uint32_t c, uint64_t d, uint32_t e), (a, b, c, d, e)) \
    X(path_filestat_get, (uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e), (a, b, c, d, e)) \
    X(path_open, (uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint64_t f, uint64_t g, uint32_t h, uint32_t i), \
      (a, b, c, d, e, f, g, h, i)) \
    X(path_create_directory, (uint32_t a, uint32_t b, uint32_t c), (a, b, c)) \
    X(path_remove_directory, (uint32_t a, uint32_t b, uint32_t c), (a, b, c)) \
    X(path_unlink_file, (uint32_t a, uint32_t b, uint32_t c), (a, b, c)) \
    X(path_rename, (uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f), (a, b, c, d, e, f)) \
    X(path_readlink, (uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f), (a, b, c, d, e, f)) \
    X(poll_oneoff, (uint32_t a, uint32_t b, uint32_t c, uint32_t d), (a, b, c, d)) \
    X(random_get, (uint32_t a, uint32_t b), (a, b))
