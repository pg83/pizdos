//@ host: false
//@ compile-flags: -g -D_GNU_SOURCE -fno-builtin
//@ max-memory: 4194304
//@ stdout: "memory exhaustion: ok\n"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <locale.h>
#include <malloc.h>
#include <net/if.h>
#include <netdb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wchar.h>

static void* large[128];
static void* small[32768];

// Diagnostics must work with no room left for stdio's buffers.
static void check(bool condition, int line) {
    if (!condition) {
        char digits[32];
        size_t at = sizeof(digits);
        digits[--at] = '\n';
        do {
            digits[--at] = '0' + line % 10;
            line /= 10;
        } while (line);
        const char prefix[] = "memory exhaustion check failed at line ";
        write(2, prefix, sizeof(prefix) - 1);
        write(2, digits + at, sizeof(digits) - at);
        _exit(99);
    }
}
#undef assert
#define assert(condition) check((condition), __LINE__)

int main() {
    assert(localeconv() != nullptr);
    int directory = open("/", O_RDONLY | O_DIRECTORY);
    assert(directory >= 0);
    auto* preserved = static_cast<unsigned char*>(malloc(16));
    assert(preserved);
    memset(preserved, 0x57, 16);
    size_t count = 0;
    while (count < 128 && (large[count] = malloc(65536))) {
        memset(large[count], 0x33, 65536);
        ++count;
    }
    assert(count > 0 && count < 128 && errno == ENOMEM);
    size_t tiny = 0;
    // Drain smaller requests too: a failed large allocation can leave
    // room for the small records that library calls need.
    for (size_t size = 16; size <= 16384; size += 16) {
        while (tiny < 32768 && (small[tiny] = malloc(size))) {
            ++tiny;
        }
        assert(tiny < 32768 && errno == ENOMEM);
    }
    assert(calloc(1, 65536) == nullptr && errno == ENOMEM);
    assert(realloc(nullptr, 65536) == nullptr && errno == ENOMEM);
    assert(realloc(preserved, 65536) == nullptr && errno == ENOMEM);
    assert(reallocarray(preserved, 2, 65536) == nullptr && errno == ENOMEM);
    assert(malloc(8192) == nullptr && errno == ENOMEM);
    for (unsigned i = 0; i < 16; ++i) {
        assert(preserved[i] == 0x57);
    }
    assert(aligned_alloc(131072, 131072) == nullptr && errno == ENOMEM);
    assert(memalign(131072, 1) == nullptr && errno == ENOMEM);
    assert(valloc(65536) == nullptr && errno == ENOMEM);
    void* aligned = preserved;
    assert(posix_memalign(&aligned, 131072, 131072) == ENOMEM && aligned == preserved);
    assert(mmap(nullptr, 65536, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED && errno == ENOMEM);
    assert(mmap(nullptr, SIZE_MAX, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED && errno == ENOMEM);
    assert(strdup("x") == nullptr && errno == ENOMEM);
    assert(getcwd(nullptr, 0) == nullptr && errno == ENOMEM);
    assert(get_current_dir_name() == nullptr && errno == ENOMEM);
    assert(canonicalize_file_name("/") == nullptr && errno == ENOMEM);
    assert(realpath("/", nullptr) == nullptr && errno == ENOMEM);
    assert(wcsdup(L"wide") == nullptr && errno == ENOMEM);
    assert(opendir("/") == nullptr && errno == ENOMEM);
    assert(fdopendir(directory) == nullptr && errno == ENOMEM);
    assert(fcntl(directory, F_GETFD) >= 0 && close(directory) == 0);
    addrinfo hints{}, *addresses = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    assert(getaddrinfo("127.0.0.1", "80", &hints, &addresses) == EAI_MEMORY && addresses == nullptr);
    assert(gethostbyname("127.0.0.1") == nullptr && h_errno == NO_RECOVERY);
    assert(getservbyname("http", "tcp") == nullptr && errno == ENOMEM);
    assert(getprotobyname("tcp") == nullptr && errno == ENOMEM);
    assert(if_nameindex() == nullptr && errno == ENOBUFS);

    char** savedEnvironment = environ;
    char entry[] = "OOM_EXISTING=value";
    char* supplied[] = {entry, nullptr};
    environ = supplied;
    char added[] = "OOM_ADDED=value";
    assert(putenv(added) == -1 && errno == ENOMEM && environ == supplied);
    assert(setenv("OOM_ADDED", "value", 1) == -1 && errno == ENOMEM && environ == supplied);
    assert(unsetenv("OOM_EXISTING") == -1 && errno == ENOMEM && environ == supplied);
    assert(strcmp(getenv("OOM_EXISTING"), "value") == 0);
    environ = savedEnvironment;

    glob_t paths{};
    paths.gl_offs = SIZE_MAX;
    assert(glob("/", GLOB_DOOFFS, nullptr, &paths) == GLOB_NOSPACE && paths.gl_pathv == nullptr);
    paths = {};
    assert(glob("/", 0, nullptr, &paths) == GLOB_NOSPACE && paths.gl_pathv == nullptr);
    // One free small slot holds the vector, leaving none for its string.
    free(small[0]);
    small[0] = nullptr;
    assert(glob("/", 0, nullptr, &paths) == GLOB_NOSPACE && paths.gl_pathv == nullptr);

    free(large[--count]);
    auto* recovered = static_cast<unsigned char*>(calloc(1, 65536));
    assert(recovered && recovered[0] == 0 && recovered[65535] == 0);
    free(recovered);
    for (size_t i = 0; i < count; ++i) {
        assert(static_cast<unsigned char*>(large[i])[65535] == 0x33);
        free(large[i]);
    }
    for (size_t i = 0; i < tiny; ++i) {
        free(small[i]);
    }
    assert(realloc(preserved, 0) == nullptr);
    recovered = static_cast<unsigned char*>(malloc(1024 * 1024));
    assert(recovered);
    recovered[1024 * 1024 - 1] = 0x75;
    assert(recovered[1024 * 1024 - 1] == 0x75);
    free(recovered);
    puts("memory exhaustion: ok");
}
