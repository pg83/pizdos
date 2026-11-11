#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <stdio.h>
#include <signal.h>
#include <poll.h>
#include <sched.h>
#include <sys/select.h>
#include <sys/times.h>
#include <sys/wait.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <unistd.h>

int main(int argc, char** argv) {
    assert(argc == 2);
    if (strcmp(argv[1], "system") == 0) {
        struct utsname names;
        char buffer[256];
        struct sysinfo memory;
        struct rusage usage;
        double load[3];
        assert(uname(&names) == -1 && errno == EIO);
        assert(gethostname(buffer, sizeof(buffer)) == -1 && errno == EIO);
        assert(getdomainname(buffer, sizeof(buffer)) == -1 && errno == EIO);
        assert(sysinfo(&memory) == -1 && errno == EIO);
        assert(get_phys_pages() == -1 && get_avphys_pages() == -1);
        assert(sysconf(_SC_PHYS_PAGES) == -1 && sysconf(_SC_AVPHYS_PAGES) == -1);
        assert(getloadavg(load, 3) == -1);
        assert(getrusage(RUSAGE_SELF, &usage) == -1 && errno == EIO);
        assert(getgroups(0, NULL) == -1 && errno == EIO);
        struct tms ticks;
        assert(times(&ticks) == (clock_t)-1 && errno == EIO);
    } else if (strcmp(argv[1], "filesystem") == 0) {
        int fd = open("/tmp/data", O_CREAT | O_TRUNC | O_RDWR, 0600);
        assert(fd >= 0 && write(fd, "before", 6) == 6 && ftruncate(fd, 4096) == 0);
        struct statfs fs;
        struct statvfs vfs;
        assert(statfs("/tmp/data", &fs) == -1 && errno == EIO);
        assert(fstatfs(fd, &fs) == -1 && errno == EIO);
        assert(statvfs("/tmp/data", &vfs) == -1 && errno == EIO);
        assert(fstatvfs(fd, &vfs) == -1 && errno == EIO);
        assert(futimens(fd, NULL) == -1 && errno == EIO);
        assert(utimensat(AT_FDCWD, "/tmp/data", NULL, 0) == -1 && errno == EIO);
        assert(syncfs(fd) == -1 && errno == EIO);
        // The filter suppresses the real host sync while the void API returns.
        sync();
        char* mapping = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        assert(mapping != MAP_FAILED && mapping[0] == 'b');
        mapping[0] = 'X';
        assert(msync(mapping, 4096, MS_SYNC) == -1 && errno == EIO);
        assert(munmap(mapping, 4096) == 0);
        char byte;
        assert(pread(fd, &byte, 1, 0) == 1 && byte == 'b');
        assert(pwrite(fd, "X", 1, 0) == -1 && errno == EIO);
        assert(close(fd) == 0 && unlink("/tmp/data") == 0);
    } else if (strcmp(argv[1], "network") == 0 || strcmp(argv[1], "retry") == 0) {
        int retry = strcmp(argv[1], "retry") == 0;
        assert(socket(AF_INET, SOCK_DGRAM, 0) == -1 && errno == (retry ? EAGAIN : EIO));
        struct addrinfo hints = {0}, *result = NULL;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        assert(getaddrinfo("syscall-failure.invalid", "80", &hints, &result) == EAI_AGAIN);
        assert(result == NULL);
        assert(gethostbyname("syscall-failure.invalid") == NULL && h_errno == TRY_AGAIN);
        if (!retry) {
            assert(if_nameindex() == NULL && errno == EIO);
            hints.ai_flags = AI_ADDRCONFIG;
            assert(getaddrinfo("127.0.0.1", "80", &hints, &result) == EAI_NONAME && result == NULL);
            hints.ai_flags = 0;
            hints.ai_protocol = IPPROTO_UDP;
            assert(getaddrinfo("127.0.0.1", "80", &hints, &result) == EAI_SOCKTYPE && result == NULL);
            struct sockaddr_in address = {0};
            address.sin_family = AF_INET;
            assert(inet_pton(AF_INET, "203.0.113.253", &address.sin_addr) == 1);
            char name[256];
            assert(getnameinfo((struct sockaddr*)&address, sizeof(address), name, sizeof(name), NULL, 0, NI_NAMEREQD) == EAI_NONAME);
            assert(gethostbyaddr(&address.sin_addr, sizeof(address.sin_addr), AF_INET) == NULL && h_errno == HOST_NOT_FOUND);
        }
    } else if (strcmp(argv[1], "socket-names") == 0) {
        int fd = socket(AF_INET, SOCK_DGRAM, 0);
        assert(fd >= 0);
        struct sockaddr_in address;
        socklen_t size = sizeof(address);
        assert(getsockname(fd, (struct sockaddr*)&address, &size) == -1 && errno == EIO);
        size = sizeof(address);
        assert(getpeername(fd, (struct sockaddr*)&address, &size) == -1 && errno == EIO);
        assert(close(fd) == 0);
    } else if (strcmp(argv[1], "connect-status") == 0 || strcmp(argv[1], "connect-poll") == 0) {
        int server = socket(AF_INET, SOCK_STREAM, 0), client = socket(AF_INET, SOCK_STREAM, 0);
        assert(server >= 0 && client >= 0);
        struct sockaddr_in address = {0};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        assert(bind(server, (struct sockaddr*)&address, sizeof(address)) == 0 && listen(server, 1) == 0);
        socklen_t size = sizeof(address);
        assert(getsockname(server, (struct sockaddr*)&address, &size) == 0);
        assert(connect(client, (struct sockaddr*)&address, size) == -1 && errno == EIO);
        assert(close(client) == 0 && close(server) == 0);
    } else if (strcmp(argv[1], "locks") == 0) {
        int fd = open("/tmp/locks", O_CREAT | O_TRUNC | O_RDWR, 0600);
        assert(fd >= 0);
        struct flock lock = {0};
        lock.l_type = F_WRLCK;
        lock.l_whence = SEEK_SET;
        lock.l_len = 4;
        assert(fcntl(fd, F_GETLK, &lock) == -1 && errno == EIO && lock.l_type == F_WRLCK);
        assert(lockf(fd, F_TEST, 4) == -1 && errno == EIO);
        assert(fcntl(fd, F_SETFL, O_APPEND) == -1 && errno == EIO);
        assert((fcntl(fd, F_GETFL) & O_APPEND) == 0);
        assert(close(fd) == 0 && unlink("/tmp/locks") == 0);
    } else if (strcmp(argv[1], "poll") == 0) {
        assert(poll(NULL, 0, 1) == -1 && errno == EIO);
        assert(poll(NULL, 65537, 0) == -1 && errno == EINVAL);
        struct timeval instant = {0};
        assert(select(0, NULL, NULL, NULL, &instant) == -1 && errno == EIO);
        struct timespec invalid = {-1, 0};
        assert(pselect(0, NULL, NULL, NULL, &invalid, NULL) == -1 && errno == EINVAL);
        int ends[2];
        assert(pipe(ends) == 0);
        char byte;
        assert(read(ends[0], &byte, 1) == -1 && errno == EIO);
        assert(write(1, "", 0) == -1 && errno == EIO);
        assert(close(ends[0]) == 0 && close(ends[1]) == 0);
        pid_t child = fork();
        assert(child >= 0);
        if (child == 0) {
            for (;;) {
                __asm__ volatile("" ::: "memory");
            }
        }
        int status;
        assert(waitpid(child, &status, 0) == -1 && errno == EIO);
        assert(kill(child, SIGKILL) == 0);
        pid_t waited;
        do {
            waited = waitpid(child, &status, WNOHANG);
            assert(waited >= 0);
            if (waited == 0) {
                sched_yield();
            }
        } while (waited == 0);
        assert(waited == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    } else if (strcmp(argv[1], "limits") == 0) {
        struct rlimit limit;
        assert(getrlimit(RLIMIT_NOFILE, &limit) == 0);
        assert(limit.rlim_cur == RLIM_INFINITY && limit.rlim_max == RLIM_INFINITY);
    } else {
        return 2;
    }
    if (strcmp(argv[1], "poll") == 0 || strcmp(argv[1], "connect-poll") == 0) {
        return 0;
    }
    puts("host syscall errors: ok");
    return 0;
}
