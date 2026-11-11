//@ compile-flags: -g -D_GNU_SOURCE
//@ stdout: "contended file locks: ok\n"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t alarms;

static void alarmed(int signal) {
    assert(signal == SIGALRM);
    alarms = alarms + 1;
}

int main() {
    alarm(10);
    int fd = open("locked.data", O_CREAT | O_TRUNC | O_RDWR, 0600);
    assert(fd >= 0 && ftruncate(fd, 16) == 0);
    struct flock lock{};
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    lock.l_len = 4;
    assert(fcntl(fd, F_SETLK, &lock) == 0 && flock(fd, LOCK_EX | LOCK_NB) == 0);
    int ready[2], release[2];
    assert(pipe(ready) == 0 && pipe(release) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        assert(close(fd) == 0);
        int other = open("locked.data", O_RDWR);
        assert(other >= 0);
        struct flock observed = lock;
        assert(fcntl(other, F_GETLK, &observed) == 0 && observed.l_type == F_WRLCK);
        assert(lockf(other, F_TEST, 4) == -1 && errno == EACCES);
        assert(fcntl(other, F_SETLK, &lock) == -1 && (errno == EAGAIN || errno == EACCES));
        assert(flock(other, LOCK_EX | LOCK_NB) == -1 && errno == EAGAIN);
        struct sigaction action{};
        action.sa_handler = alarmed;
        assert(sigemptyset(&action.sa_mask) == 0 && sigaction(SIGALRM, &action, nullptr) == 0);
        for (unsigned operation = 0; operation < 3; ++operation) {
            sig_atomic_t before = alarms;
            ualarm(20000, 0);
            int result;
            if (operation == 0) {
                result = fcntl(other, F_SETLKW, &lock);
            } else if (operation == 1) {
                result = lockf(other, F_LOCK, 4);
            } else {
                result = flock(other, LOCK_EX);
            }
            assert(result == -1 && errno == EINTR && alarms == before + 1);
            ualarm(0, 0);
        }
        assert(write(ready[1], "r", 1) == 1);
        char byte;
        assert(read(release[0], &byte, 1) == 1 && byte == 'x');
        assert(lockf(other, F_TEST, 4) == 0);
        assert(fcntl(other, F_SETLK, &lock) == 0 && flock(other, LOCK_EX | LOCK_NB) == 0);
        assert(close(other) == 0);
        _exit(17);
    }
    char byte;
    assert(read(ready[0], &byte, 1) == 1 && byte == 'r');
    lock.l_type = F_UNLCK;
    assert(fcntl(fd, F_SETLK, &lock) == 0 && flock(fd, LOCK_UN) == 0);
    assert(write(release[1], "x", 1) == 1);
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 17);
    assert(close(fd) == 0 && unlink("locked.data") == 0);
    for (int end : ready) {
        assert(close(end) == 0);
    }
    for (int end : release) {
        assert(close(end) == 0);
    }
    alarm(0);
    puts("contended file locks: ok");
}
