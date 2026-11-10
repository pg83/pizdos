#pragma once

#include <__base.h>
#include <sched.h>
#include <time.h>

__BEGIN_DECLS

/*
 * A process has one thread: mutexes, condition variables, read-write locks,
 * once and keys are lib/c's own and never wait for another thread; a thread
 * is never created.
 */
typedef unsigned long pthread_t;
typedef unsigned pthread_key_t;
typedef int pthread_once_t;

typedef struct {
    int __type;
} pthread_mutexattr_t;

typedef struct {
    int __type;
    int __locks;
} pthread_mutex_t;

typedef struct {
    int __unused;
} pthread_condattr_t;

typedef struct {
    int __unused;
} pthread_cond_t;

typedef struct {
    int __unused;
} pthread_rwlockattr_t;

typedef struct {
    int __readers;
    int __writer;
} pthread_rwlock_t;

typedef struct {
    int __detached;
    size_t __stacksize;
} pthread_attr_t;

#define PTHREAD_MUTEX_NORMAL 0
#define PTHREAD_MUTEX_RECURSIVE 1
#define PTHREAD_MUTEX_ERRORCHECK 2
#define PTHREAD_MUTEX_DEFAULT PTHREAD_MUTEX_NORMAL

#define PTHREAD_CREATE_JOINABLE 0
#define PTHREAD_CREATE_DETACHED 1

#define PTHREAD_MUTEX_INITIALIZER {PTHREAD_MUTEX_NORMAL, 0}
#define PTHREAD_COND_INITIALIZER {0}
#define PTHREAD_RWLOCK_INITIALIZER {0, 0}
#define PTHREAD_ONCE_INIT 0

#define PTHREAD_KEYS_MAX 128

int pthread_create(pthread_t* __restrict, const pthread_attr_t* __restrict, void* (*)(void*), void* __restrict);
int pthread_join(pthread_t, void**);
int pthread_detach(pthread_t);
pthread_t pthread_self(void);
int pthread_equal(pthread_t, pthread_t);
int pthread_once(pthread_once_t*, void (*)(void));

int pthread_attr_init(pthread_attr_t*);
int pthread_attr_destroy(pthread_attr_t*);
int pthread_attr_setdetachstate(pthread_attr_t*, int);
int pthread_attr_getdetachstate(const pthread_attr_t*, int*);
int pthread_attr_setstacksize(pthread_attr_t*, size_t);
int pthread_attr_getstacksize(const pthread_attr_t* __restrict, size_t* __restrict);

int pthread_key_create(pthread_key_t*, void (*)(void*));
int pthread_key_delete(pthread_key_t);
void* pthread_getspecific(pthread_key_t);
int pthread_setspecific(pthread_key_t, const void*);

int pthread_mutexattr_init(pthread_mutexattr_t*);
int pthread_mutexattr_destroy(pthread_mutexattr_t*);
int pthread_mutexattr_settype(pthread_mutexattr_t*, int);
int pthread_mutexattr_gettype(const pthread_mutexattr_t* __restrict, int* __restrict);

int pthread_mutex_init(pthread_mutex_t* __restrict, const pthread_mutexattr_t* __restrict);
int pthread_mutex_destroy(pthread_mutex_t*);
int pthread_mutex_lock(pthread_mutex_t*);
int pthread_mutex_trylock(pthread_mutex_t*);
int pthread_mutex_timedlock(pthread_mutex_t* __restrict, const struct timespec* __restrict);
int pthread_mutex_unlock(pthread_mutex_t*);

int pthread_condattr_init(pthread_condattr_t*);
int pthread_condattr_destroy(pthread_condattr_t*);
int pthread_cond_init(pthread_cond_t* __restrict, const pthread_condattr_t* __restrict);
int pthread_cond_destroy(pthread_cond_t*);
int pthread_cond_signal(pthread_cond_t*);
int pthread_cond_broadcast(pthread_cond_t*);
int pthread_cond_wait(pthread_cond_t* __restrict, pthread_mutex_t* __restrict);
int pthread_cond_timedwait(pthread_cond_t* __restrict, pthread_mutex_t* __restrict, const struct timespec* __restrict);

int pthread_rwlock_init(pthread_rwlock_t* __restrict, const pthread_rwlockattr_t* __restrict);
int pthread_rwlock_destroy(pthread_rwlock_t*);
int pthread_rwlock_rdlock(pthread_rwlock_t*);
int pthread_rwlock_tryrdlock(pthread_rwlock_t*);
int pthread_rwlock_wrlock(pthread_rwlock_t*);
int pthread_rwlock_trywrlock(pthread_rwlock_t*);
int pthread_rwlock_unlock(pthread_rwlock_t*);

__END_DECLS
