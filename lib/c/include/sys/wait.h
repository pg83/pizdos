#pragma once

#include <__base.h>
#include <signal.h>

__BEGIN_DECLS

typedef enum {
    P_ALL = 0,
    P_PID = 1,
    P_PGID = 2,
} idtype_t;

#define WNOHANG 1
#define WUNTRACED 2
#define WSTOPPED 2
#define WEXITED 4
#define WCONTINUED 8
#define WNOWAIT 0x1000000

#define WEXITSTATUS(s) (((s) & 0xff00) >> 8)
#define WTERMSIG(s) ((s) & 0x7f)
#define WSTOPSIG(s) WEXITSTATUS(s)
#define WCOREDUMP(s) ((s) & 0x80)
#define WIFEXITED(s) (!WTERMSIG(s))
#define WIFSTOPPED(s) ((short)((((s) & 0xffff) * 0x10001U) >> 8) > 0x7f00)
#define WIFSIGNALED(s) (((s) & 0xffff) - 1U < 0xffu)
#define WIFCONTINUED(s) ((s) == 0xffff)

struct rusage;

pid_t wait(int*);
pid_t waitpid(pid_t, int*, int);
pid_t wait3(int*, int, struct rusage*);
pid_t wait4(pid_t, int*, int, struct rusage*);
int waitid(idtype_t, id_t, siginfo_t*, int);

__END_DECLS
