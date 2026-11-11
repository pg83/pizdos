//@ host: false
//@ freestanding: true
//@ args: $ROOT/tmp
//@ stdout: "wasi filesystem: ok\n"

using u8 = __UINT8_TYPE__;
using u32 = __UINT32_TYPE__;
using u64 = __UINT64_TYPE__;

#define WASI(name) __attribute__((import_module("wasi_snapshot_preview1"), import_name(#name)))
struct IoVec {
    const void* data;
    u32 size;
};

extern "C" {
u32 args_sizes_get(u32*, u32*) WASI(args_sizes_get);
u32 args_get(char**, char*) WASI(args_get);
u32 environ_sizes_get(u32*, u32*) WASI(environ_sizes_get);
u32 environ_get(char**, char*) WASI(environ_get);
u32 clock_time_get(u32, u64, u64*) WASI(clock_time_get);
u32 clock_res_get(u32, u64*) WASI(clock_res_get);
u32 random_get(void*, u32) WASI(random_get);
u32 sched_yield() WASI(sched_yield);
u32 fd_write(u32, const IoVec*, u32, u32*) WASI(fd_write);
u32 fd_read(u32, const IoVec*, u32, u32*) WASI(fd_read);
u32 fd_pwrite(u32, const IoVec*, u32, u64, u32*) WASI(fd_pwrite);
u32 fd_pread(u32, const IoVec*, u32, u64, u32*) WASI(fd_pread);
u32 fd_close(u32) WASI(fd_close);
u32 fd_seek(u32, u64, u32, u64*) WASI(fd_seek);
u32 fd_sync(u32) WASI(fd_sync);
u32 fd_fdstat_get(u32, void*) WASI(fd_fdstat_get);
u32 fd_fdstat_set_flags(u32, u32) WASI(fd_fdstat_set_flags);
u32 fd_filestat_get(u32, void*) WASI(fd_filestat_get);
u32 fd_filestat_set_size(u32, u64) WASI(fd_filestat_set_size);
u32 fd_prestat_get(u32, void*) WASI(fd_prestat_get);
u32 fd_prestat_dir_name(u32, char*, u32) WASI(fd_prestat_dir_name);
u32 fd_readdir(u32, void*, u32, u64, u32*) WASI(fd_readdir);
u32 path_open(u32, u32, const char*, u32, u32, u64, u64, u32, u32*) WASI(path_open);
u32 path_filestat_get(u32, u32, const char*, u32, void*) WASI(path_filestat_get);
u32 path_create_directory(u32, const char*, u32) WASI(path_create_directory);
u32 path_remove_directory(u32, const char*, u32) WASI(path_remove_directory);
u32 path_unlink_file(u32, const char*, u32) WASI(path_unlink_file);
u32 path_rename(u32, const char*, u32, u32, const char*, u32) WASI(path_rename);
u32 path_readlink(u32, const char*, u32, char*, u32, u32*) WASI(path_readlink);
u32 poll_oneoff(const void*, void*, u32, u32*) WASI(poll_oneoff);
[[noreturn]] void proc_exit(u32) WASI(proc_exit);
}

#define CHECK(condition) do { if (!(condition)) { proc_exit(__LINE__ % 250 + 1); } } while (false)

static u32 length(const char* value) {
    u32 n = 0;
    while (value[n]) {
        ++n;
    }
    return n;
}

static u64 integer(const u8* value) {
    u64 result = 0;
    for (u32 i = 0; i < 8; ++i) {
        result |= u64(value[i]) << (8 * i);
    }
    return result;
}

struct Subscription {
    u64 userdata;
    u8 type;
    u8 padding[7];
    u32 clock;
    u32 reserved;
    u64 timeout;
    u64 precision;
    u64 flags;
};

extern "C" __attribute__((export_name("_start"))) void _start() {
    static_assert(sizeof(Subscription) == 48);
    auto bad = reinterpret_cast<void*>(0xffffff00u);
    auto bad32 = static_cast<u32*>(bad);
    u32 count, size, used, directory, file, other;
    char* arguments[1024];
    char strings[131072];
    char environment[131072];
    u8 status[64];
    char buffer[4096];
    CHECK(args_sizes_get(&count, &size) == 0 && count == 2 && size < sizeof(strings));
    CHECK(args_get(arguments, strings) == 0);
    const char* root = arguments[1];
    CHECK(args_sizes_get(bad32, &size) == 21);
    CHECK(args_get(reinterpret_cast<char**>(bad), strings) == 21);
    CHECK(environ_sizes_get(&count, &size) == 0 && count < 1024 && size < sizeof(strings));
    CHECK(environ_get(arguments, environment) == 0);
    CHECK(environ_sizes_get(&count, bad32) == 21);
    CHECK(environ_get(reinterpret_cast<char**>(bad), strings) == 21);
    u64 now = 0;
    for (u32 clock = 0; clock < 4; ++clock) {
        CHECK(clock_time_get(clock, 0, &now) == 0);
        CHECK(clock_res_get(clock, &now) == 0 && now > 0);
    }
    CHECK(clock_time_get(99, 0, &now) == 28);
    CHECK(clock_res_get(99, &now) == 28);
    CHECK(clock_time_get(0, 0, static_cast<u64*>(bad)) == 21);
    CHECK(clock_res_get(0, static_cast<u64*>(bad)) == 21);
    CHECK(random_get(buffer, 64) == 0 && random_get(buffer, 0) == 0);
    CHECK(random_get(bad, 8) == 21 && sched_yield() == 0);
    CHECK(fd_prestat_get(3, status) == 0 && integer(status) == (u64(1) << 32));
    CHECK(fd_prestat_get(99, status) == 8 && fd_prestat_get(3, bad) == 21);
    CHECK(fd_prestat_dir_name(3, buffer, 1) == 0 && buffer[0] == '/');
    CHECK(fd_prestat_dir_name(99, buffer, 1) == 8);
    CHECK(fd_prestat_dir_name(3, buffer, 0) == 28 && fd_prestat_dir_name(3, static_cast<char*>(bad), 1) == 21);
    constexpr u64 rights = (u64(1) << 1) | (u64(1) << 6);
    CHECK(path_open(3, 1, root, length(root), 2, rights, rights, 0, &directory) == 0);
    CHECK(fd_fdstat_get(directory, status) == 0 && status[0] == 3);
    CHECK(fd_fdstat_get(directory, bad) == 21 && fd_fdstat_get(99, status) == 8);
    CHECK(fd_readdir(directory, buffer, sizeof(buffer), 0, &used) == 0 && used > 0);
    CHECK(fd_readdir(directory, buffer, 1, 0, &used) == 0 && used == 1);
    CHECK(fd_readdir(directory, buffer, sizeof(buffer), u64(1) << 60, &used) == 0 && used == 0);
    CHECK(fd_readdir(99, buffer, sizeof(buffer), 0, &used) == 8);
    CHECK(fd_readdir(directory, bad, 8, 0, &used) == 21);
    CHECK(fd_readdir(directory, buffer, 8, 0, bad32) == 21);
    CHECK(path_open(directory, 1, "data", 4, 1 | 4 | 8, rights, rights, 0, &file) == 0);
    CHECK(fd_fdstat_set_flags(file, 1 | 4) == 0);
    CHECK(fd_fdstat_get(file, status) == 0 && (status[2] & 5) == 5);
    CHECK(fd_fdstat_set_flags(file, 0) == 0 && fd_fdstat_set_flags(99, 0) == 8);
    IoVec vectors[] = {{"abc", 3}, {"DEF", 3}};
    CHECK(fd_write(file, vectors, 2, &used) == 0 && used == 6);
    IoVec one = {"Z", 1};
    CHECK(fd_pwrite(file, &one, 1, 2, &used) == 0 && used == 1);
    u64 offset = 0;
    CHECK(fd_seek(file, 0, 0, &offset) == 0 && offset == 0);
    one = {buffer, 6};
    CHECK(fd_read(file, &one, 1, &used) == 0 && used == 6);
    CHECK(buffer[0] == 'a' && buffer[1] == 'b' && buffer[2] == 'Z' && buffer[5] == 'F');
    CHECK(fd_pread(file, &one, 1, 1, &used) == 0 && used == 5 && buffer[0] == 'b');
    CHECK(fd_read(99, &one, 1, &used) == 8);
    CHECK(fd_write(file, &one, 1025, &used) == 28);
    CHECK(fd_write(file, static_cast<IoVec*>(bad), 1, &used) == 21);
    CHECK(fd_write(file, &one, 1, bad32) == 21);
    one = {bad, 8};
    CHECK(fd_write(file, &one, 1, &used) == 21);
    CHECK(fd_seek(99, 0, 0, &offset) == 8 && fd_seek(file, 0, 99, &offset) == 28);
    CHECK(fd_seek(file, 0, 0, static_cast<u64*>(bad)) == 21);
    CHECK(fd_seek(file, ~u64(0), 0, &offset) == 28);
    CHECK(fd_readdir(file, buffer, sizeof(buffer), 0, &used) == 54);
    CHECK(fd_sync(file) == 0 && fd_sync(99) == 8);
    CHECK(fd_filestat_get(file, status) == 0 && status[16] == 4 && integer(status + 32) == 6);
    CHECK(fd_filestat_get(file, bad) == 21 && fd_filestat_get(99, status) == 8);
    CHECK(fd_filestat_set_size(file, 2) == 0 && fd_filestat_set_size(99, 0) == 8);
    CHECK(path_filestat_get(directory, 1, "data", 4, status) == 0 && integer(status + 32) == 2);
    CHECK(path_filestat_get(directory, 0, "", 0, status) == 0 && status[16] == 3);
    CHECK(path_filestat_get(99, 0, "data", 4, status) == 8);
    CHECK(path_filestat_get(directory, 0, static_cast<char*>(bad), 4, status) == 21);
    CHECK(path_filestat_get(directory, 0, buffer, 4096, status) == 37);
    CHECK(path_filestat_get(directory, 0, "data", 4, bad) == 21);
    CHECK(path_open(99, 1, "data", 4, 0, rights, rights, 0, &other) == 8);
    CHECK(path_open(directory, 1, "data", 4, 0, rights, rights, 0, bad32) == 21);
    CHECK(path_open(directory, 1, "data", 4, 1 | 4, rights, rights, 0, &other) == 20);
    CHECK(path_open(directory, 1, "missing", 7, 0, rights, rights, 0, &other) == 44);
    CHECK(path_open(directory, 1, "data", 4, 0, u64(1) << 1, 0, 0, &other) == 0);
    one = {"X", 1};
    CHECK(fd_write(other, &one, 1, &used) == 8 && fd_close(other) == 0);
    CHECK(path_open(directory, 1, "data", 4, 0, u64(1) << 6, 0, 1 | 4, &other) == 0);
    CHECK(fd_fdstat_get(other, status) == 0 && (status[2] & 5) == 5);
    one = {buffer, 1};
    CHECK(fd_read(other, &one, 1, &used) == 8 && fd_close(other) == 0);
    CHECK(path_open(directory, 0, "data", 4, 0, 0, 0, 0, &other) == 0 && fd_close(other) == 0);
    CHECK(path_filestat_get(directory, 1, "missing", 7, status) == 44);
    CHECK(path_open(directory, 1, static_cast<char*>(bad), 4, 0, rights, 0, 0, &other) == 21);
    CHECK(path_create_directory(directory, static_cast<char*>(bad), 4) == 21);
    CHECK(path_remove_directory(directory, static_cast<char*>(bad), 4) == 21);
    CHECK(path_unlink_file(directory, static_cast<char*>(bad), 4) == 21);
    CHECK(path_rename(directory, static_cast<char*>(bad), 4, directory, "data", 4) == 21);
    CHECK(path_rename(directory, "data", 4, directory, static_cast<char*>(bad), 4) == 21);
    CHECK(path_readlink(directory, static_cast<char*>(bad), 4, buffer, sizeof(buffer), &used) == 21);
    CHECK(path_open(3, 1, "/dev/null", 9, 0, rights, 0, 0, &other) == 0);
    CHECK(fd_fdstat_get(other, status) == 0 && status[0] == 2 && fd_close(other) == 0);
    CHECK(fd_fdstat_get(1, status) == 0 && status[0] == 0);
    CHECK(fd_readdir(1, buffer, sizeof(buffer), 0, &used) == 70);
    CHECK(path_filestat_get(3, 0, "/proc/self/fd/0", 15, status) == 0 && status[16] == 7);
    CHECK(path_readlink(3, "/proc/self/fd/0", 15, buffer, sizeof(buffer), &used) == 0 && used > 0);
    char first = buffer[0];
    CHECK(path_readlink(3, "/proc/self/fd/0", 15, buffer, 1, &used) == 0 && used == 1 && buffer[0] == first);
    CHECK(path_open(3, 1, "/proc/self/fd", 13, 2, 0, 0, 0, &other) == 0);
    CHECK(fd_readdir(other, buffer, sizeof(buffer), 0, &used) == 0 && fd_close(other) == 0);
    bool sawLink = false;
    for (u32 at = 0; at + 24 <= used;) {
        auto entry = reinterpret_cast<u8*>(buffer + at);
        u32 size = u32(integer(entry + 16));
        CHECK(at + 24 + size <= used);
        sawLink = sawLink || entry[20] == 7;
        at += 24 + size;
    }
    CHECK(sawLink);
    CHECK(path_create_directory(directory, "sub", 3) == 0);
    CHECK(fd_readdir(directory, buffer, sizeof(buffer), 0, &used) == 0);
    bool sawFile = false, sawDirectory = false;
    for (u32 at = 0; at + 24 <= used;) {
        auto entry = reinterpret_cast<u8*>(buffer + at);
        u32 size = u32(integer(entry + 16));
        CHECK(at + 24 + size <= used);
        if (size == 4 && buffer[at + 24] == 'd' && buffer[at + 25] == 'a' && buffer[at + 26] == 't' && buffer[at + 27] == 'a') {
            CHECK(entry[20] == 4);
            sawFile = true;
        }
        if (size == 3 && buffer[at + 24] == 's' && buffer[at + 25] == 'u' && buffer[at + 26] == 'b') {
            CHECK(entry[20] == 3);
            sawDirectory = true;
        }
        at += 24 + size;
    }
    CHECK(sawFile && sawDirectory);
    CHECK(path_create_directory(directory, "sub", 3) == 20);
    CHECK(path_create_directory(99, "sub", 3) == 8);
    CHECK(path_open(directory, 1, "sub/item", 8, 1, rights, rights, 0, &other) == 0);
    CHECK(path_remove_directory(directory, "sub", 3) == 55);
    CHECK(path_unlink_file(directory, "sub/item", 8) == 0 && fd_close(other) == 0);
    CHECK(path_remove_directory(directory, "sub", 3) == 0);
    CHECK(path_remove_directory(99, "sub", 3) == 8);
    CHECK(path_rename(directory, "data", 4, directory, "renamed", 7) == 0);
    CHECK(path_rename(99, "data", 4, directory, "renamed", 7) == 8);
    CHECK(path_readlink(directory, "renamed", 7, buffer, sizeof(buffer), &used) == 28);
    CHECK(path_readlink(99, "renamed", 7, buffer, sizeof(buffer), &used) == 8);
    CHECK(path_readlink(directory, "renamed", 7, static_cast<char*>(bad), 8, &used) == 21);
    CHECK(path_unlink_file(directory, "renamed", 7) == 0 && fd_close(file) == 0);
    CHECK(path_unlink_file(directory, "renamed", 7) == 44 && path_unlink_file(99, "renamed", 7) == 8);
    CHECK(fd_close(file) == 8 && fd_close(directory) == 0 && fd_close(3) == 0);

    Subscription subscription{0x123456789abcdef0ULL, 1, {}, 0, 0, 0, 0, 0};
    u8 events[32]{};
    CHECK(poll_oneoff(&subscription, events, 1, &used) == 0 && used == 1 && events[10] == 1);
    CHECK(integer(events) == subscription.userdata);
    subscription.type = 0;
    subscription.clock = 1;
    CHECK(poll_oneoff(&subscription, events, 1, &used) == 0 && used == 1);
    subscription.flags = 1;
    CHECK(clock_time_get(1, 0, &subscription.timeout) == 0);
    CHECK(poll_oneoff(&subscription, events, 1, &used) == 0);
    subscription.clock = 99;
    CHECK(poll_oneoff(&subscription, events, 1, &used) == 28);
    CHECK(poll_oneoff(&subscription, events, 0, &used) == 28);
    CHECK(poll_oneoff(bad, events, 1, &used) == 21);
    CHECK(poll_oneoff(&subscription, bad, 1, &used) == 21);
    CHECK(poll_oneoff(&subscription, events, 1, bad32) == 21);
    const char message[] = "wasi filesystem: ok\n";
    one = {message, sizeof(message) - 1};
    CHECK(fd_write(1, &one, 1, &used) == 0 && used == sizeof(message) - 1);
    proc_exit(0);
}
