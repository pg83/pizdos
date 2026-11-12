#include <assert.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <stdio.h>
#include <signal.h>
#include <poll.h>
#include <pty.h>
#include <sched.h>
#include <sys/select.h>
#include <sys/times.h>
#include <sys/wait.h>
#include <time.h>
#include <termios.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <sys/un.h>
#include <unistd.h>

int main(int argc, char** argv) {
    assert(argc == 2);
    if (strcmp(argv[1], "permanent-dns-error") == 0) {
        struct addrinfo hints = {0}, *result = NULL;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        assert(getaddrinfo("permanent-error.invalid.", "80", &hints, &result) == EAI_FAIL && result == NULL);
        assert(gethostbyname("permanent-error.invalid.") == NULL && h_errno == NO_RECOVERY);
    } else if (strcmp(argv[1], "retry-open") == 0) {
        int fd = open("/tmp/retry", O_CREAT | O_TRUNC | O_RDWR, 0600);
        assert(fd >= 0 && write(fd, "x", 1) == 1);
        char byte;
        assert(pread(fd, &byte, 1, 0) == 1 && byte == 'x');
        assert(close(fd) == 0 && unlink("/tmp/retry") == 0);
    } else if (strcmp(argv[1], "retry-read") == 0) {
        char byte;
        assert(read(0, &byte, 1) == 1 && byte == 'x');
        assert(read(0, &byte, 1) == 0);
    } else if (strcmp(argv[1], "retry-connect") == 0) {
        int listener = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in address = {0};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        assert(listener >= 0 && bind(listener, (struct sockaddr*)&address, sizeof(address)) == 0);
        socklen_t size = sizeof(address);
        assert(getsockname(listener, (struct sockaddr*)&address, &size) == 0 && listen(listener, 1) == 0);
        int client = socket(AF_INET, SOCK_STREAM, 0);
        assert(client >= 0 && connect(client, (struct sockaddr*)&address, size) == 0);
        int accepted = accept(listener, NULL, NULL);
        assert(accepted >= 0 && send(client, "x", 1, 0) == 1);
        char byte;
        assert(recv(accepted, &byte, 1, 0) == 1 && byte == 'x');
        assert(close(client) == 0 && close(accepted) == 0 && close(listener) == 0);
    } else if (strcmp(argv[1], "host-abstract") == 0 || strcmp(argv[1], "host-pathname") == 0) {
        struct sockaddr_un address;
        socklen_t length = sizeof(address);
        memset(&address, 0xff, sizeof(address));
        assert(getsockname(0, (struct sockaddr*)&address, &length) == 0 && address.sun_family == AF_UNIX);
        if (strcmp(argv[1], "host-abstract") == 0) {
            assert(length > 15 && address.sun_path[0] == 0);
            assert(memcmp(address.sun_path + 1, "pizdos-host-", 12) == 0);
        } else {
            assert(strcmp(address.sun_path, "host.socket") == 0);
        }
    } else if (strcmp(argv[1], "long-realpath") == 0) {
        char buffer[4096];
        memset(buffer, 0x5a, sizeof(buffer));
        assert(realpath("/jump/more/leaf", buffer) == NULL && errno == ENAMETOOLONG);
        for (unsigned i = 0; i < sizeof(buffer); ++i) {
            assert(buffer[i] == 0x5a);
        }
        char* allocated = realpath("/jump/more/leaf", NULL);
        assert(allocated == NULL && errno == ENAMETOOLONG);
    } else if (strcmp(argv[1], "exec-wasi") == 0) {
        char* arguments[] = {"wasi", NULL};
        assert(execv("/wasi.o", arguments) == -1 && errno == ENOEXEC);
    } else if (strcmp(argv[1], "recv-message") == 0) {
        int ends[2];
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, ends) == 0);
        assert(write(ends[0], "r", 1) == 1);
        char buffer[8];
        memset(buffer, 0x5a, sizeof(buffer));
        struct iovec vector = {buffer, sizeof(buffer)};
        struct msghdr message = {0};
        message.msg_iov = &vector;
        message.msg_iovlen = 1;
        assert(recvmsg(ends[1], &message, 0) == -1 && errno == EIO);
        for (unsigned i = 0; i < sizeof(buffer); ++i) {
            assert(buffer[i] == 0x5a);
        }
        assert(close(ends[0]) == 0 && close(ends[1]) == 0);
    } else if (strcmp(argv[1], "system") == 0) {
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
    } else if (strcmp(argv[1], "pty-attributes") == 0) {
        struct termios attributes = {0};
        attributes.c_cflag = CS8 | CREAD;
        for (int attempt = 0; attempt != 40; ++attempt) {
            int master = -1, slave = -1;
            assert(openpty(&master, &slave, NULL, &attributes, NULL) == -1 && errno == EIO);
            assert(master == -1 && slave == -1);
        }
        int master = -1, slave = -1;
        assert(openpty(&master, &slave, NULL, NULL, NULL) == 0);
        assert(close(master) == 0 && close(slave) == 0);
    } else if (strcmp(argv[1], "nonblocking-input") == 0) {
        assert(fcntl(0, F_GETFL) & O_NONBLOCK);
        char byte = 0;
        assert(read(0, &byte, 1) == -1 && errno == EAGAIN && byte == 0);
        struct pollfd input = {0, POLLIN, 0};
        assert(poll(&input, 1, 0) == 0 && input.revents == 0);
    } else if (strcmp(argv[1], "descriptor-flags") == 0) {
        assert(fcntl(0, F_GETFL) == -1 && errno == EIO);
        assert(fcntl(0, F_SETFL, 0) == -1 && errno == EIO);
    } else if (strcmp(argv[1], "access") == 0) {
        assert(access("/", F_OK) == -1 && errno == EIO);
        assert(euidaccess("/", R_OK) == -1 && errno == EIO);
        assert(faccessat(AT_FDCWD, "/", W_OK, AT_EACCESS) == -1 && errno == EIO);
        assert(chdir("/") == -1 && errno == EIO);
    } else if (strcmp(argv[1], "temp-collision") == 0) {
        char path[] = "/tmp/collision-XXXXXX";
        assert(mktemp(path) == path && path[0] != 0);
        assert(access(path, F_OK) == -1 && errno == ENOENT);
        DIR* directory = opendir("/tmp");
        assert(directory != NULL);
        unsigned occupied = 0;
        struct dirent* entry;
        while ((entry = readdir(directory)) != NULL) {
            if (strncmp(entry->d_name, "collision-", 10) != 0) {
                continue;
            }
            char existing[512];
            assert(snprintf(existing, sizeof(existing), "/tmp/%s", entry->d_name) < (int)sizeof(existing));
            assert(strcmp(path, existing) != 0);
            int fd = open(existing, O_RDONLY);
            char contents[9] = {0};
            assert(fd >= 0 && read(fd, contents, sizeof(contents)) == 8 && strcmp(contents, "occupied") == 0);
            assert(close(fd) == 0 && unlink(existing) == 0);
            ++occupied;
        }
        assert(closedir(directory) == 0 && occupied == 1);
        int fresh = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
        assert(fresh >= 0 && write(fresh, "fresh", 5) == 5);
        assert(close(fresh) == 0 && unlink(path) == 0);
    } else if (strcmp(argv[1], "temp-files") == 0) {
        char path[] = "/tmp/collision-XXXXXX";
        assert(mkstemp(path) == -1 && errno == EEXIST);
        assert(strstr(path, "XXXXXX") == NULL);
    } else if (strcmp(argv[1], "temp-directories") == 0) {
        char path[] = "/tmp/collision-XXXXXX";
        assert(mkdtemp(path) == NULL && errno == EEXIST);
        assert(strstr(path, "XXXXXX") == NULL);
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
