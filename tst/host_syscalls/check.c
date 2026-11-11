#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
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
    } else if (strcmp(argv[1], "limits") == 0) {
        struct rlimit limit;
        assert(getrlimit(RLIMIT_NOFILE, &limit) == 0);
        assert(limit.rlim_cur == RLIM_INFINITY && limit.rlim_max == RLIM_INFINITY);
    } else {
        return 2;
    }
    puts("host syscall errors: ok");
    return 0;
}
