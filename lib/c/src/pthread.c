#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

/*
 * The process's one thread. A lock it already holds cannot be released by
 * another thread, so where POSIX would block forever this returns EDEADLK;
 * a timed wait sleeps to its deadline.
 */
enum {
    self = 1,
};

static struct {
    int used;
    const void* value;
} keys[PTHREAD_KEYS_MAX];

// sleepUntil sleeps until the CLOCK_REALTIME time at.
static void sleepUntil(const struct timespec* at) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    struct timespec left = {at->tv_sec - now.tv_sec, at->tv_nsec - now.tv_nsec};

    if (left.tv_nsec < 0) {
        left.tv_sec--;
        left.tv_nsec += 1000000000;
    }

    if (left.tv_sec >= 0) {
        nanosleep(&left, 0);
    }
}

int pthread_create(pthread_t* restrict thread, const pthread_attr_t* restrict attr, void* (*start)(void*), void* restrict arg) {
    return EAGAIN;
}

int pthread_join(pthread_t thread, void** result) {
    return thread == self ? EDEADLK : ESRCH;
}

int pthread_detach(pthread_t thread) {
    return thread == self ? 0 : ESRCH;
}

pthread_t pthread_self(void) {
    return self;
}

int pthread_equal(pthread_t a, pthread_t b) {
    return a == b;
}

int pthread_once(pthread_once_t* once, void (*init)(void)) {
    if (*once == 0) {
        *once = 1;
        init();
    }

    return 0;
}

int pthread_attr_init(pthread_attr_t* attr) {
    attr->__detached = PTHREAD_CREATE_JOINABLE;
    attr->__stacksize = 0;

    return 0;
}

int pthread_attr_destroy(pthread_attr_t* attr) {
    return 0;
}

int pthread_attr_setdetachstate(pthread_attr_t* attr, int state) {
    if (state != PTHREAD_CREATE_JOINABLE && state != PTHREAD_CREATE_DETACHED) {
        return EINVAL;
    }

    attr->__detached = state;

    return 0;
}

int pthread_attr_getdetachstate(const pthread_attr_t* attr, int* state) {
    *state = attr->__detached;

    return 0;
}

int pthread_attr_setstacksize(pthread_attr_t* attr, size_t size) {
    attr->__stacksize = size;

    return 0;
}

int pthread_attr_getstacksize(const pthread_attr_t* restrict attr, size_t* restrict size) {
    *size = attr->__stacksize;

    return 0;
}

int pthread_key_create(pthread_key_t* key, void (*destructor)(void*)) {
    for (unsigned k = 0; k < PTHREAD_KEYS_MAX; k++) {
        if (!keys[k].used) {
            keys[k].used = 1;
            keys[k].value = 0;
            *key = k;

            return 0;
        }
    }

    return EAGAIN;
}

int pthread_key_delete(pthread_key_t key) {
    if (key >= PTHREAD_KEYS_MAX || !keys[key].used) {
        return EINVAL;
    }

    keys[key].used = 0;

    return 0;
}

void* pthread_getspecific(pthread_key_t key) {
    return key < PTHREAD_KEYS_MAX && keys[key].used ? (void*)keys[key].value : 0;
}

int pthread_setspecific(pthread_key_t key, const void* value) {
    if (key >= PTHREAD_KEYS_MAX || !keys[key].used) {
        return EINVAL;
    }

    keys[key].value = value;

    return 0;
}

int pthread_mutexattr_init(pthread_mutexattr_t* attr) {
    attr->__type = PTHREAD_MUTEX_DEFAULT;

    return 0;
}

int pthread_mutexattr_destroy(pthread_mutexattr_t* attr) {
    return 0;
}

int pthread_mutexattr_settype(pthread_mutexattr_t* attr, int type) {
    if (type != PTHREAD_MUTEX_NORMAL && type != PTHREAD_MUTEX_RECURSIVE && type != PTHREAD_MUTEX_ERRORCHECK) {
        return EINVAL;
    }

    attr->__type = type;

    return 0;
}

int pthread_mutexattr_gettype(const pthread_mutexattr_t* restrict attr, int* restrict type) {
    *type = attr->__type;

    return 0;
}

int pthread_mutex_init(pthread_mutex_t* restrict mutex, const pthread_mutexattr_t* restrict attr) {
    mutex->__type = attr ? attr->__type : PTHREAD_MUTEX_DEFAULT;
    mutex->__locks = 0;

    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t* mutex) {
    return mutex->__locks ? EBUSY : 0;
}

int pthread_mutex_trylock(pthread_mutex_t* mutex) {
    if (mutex->__locks && mutex->__type != PTHREAD_MUTEX_RECURSIVE) {
        return EBUSY;
    }

    mutex->__locks++;

    return 0;
}

int pthread_mutex_lock(pthread_mutex_t* mutex) {
    return pthread_mutex_trylock(mutex) == EBUSY ? EDEADLK : 0;
}

int pthread_mutex_timedlock(pthread_mutex_t* restrict mutex, const struct timespec* restrict at) {
    if (pthread_mutex_trylock(mutex) == 0) {
        return 0;
    }

    sleepUntil(at);

    return ETIMEDOUT;
}

int pthread_mutex_unlock(pthread_mutex_t* mutex) {
    if (!mutex->__locks) {
        return EPERM;
    }

    mutex->__locks--;

    return 0;
}

int pthread_condattr_init(pthread_condattr_t* attr) {
    return 0;
}

int pthread_condattr_destroy(pthread_condattr_t* attr) {
    return 0;
}

int pthread_cond_init(pthread_cond_t* restrict cond, const pthread_condattr_t* restrict attr) {
    return 0;
}

int pthread_cond_destroy(pthread_cond_t* cond) {
    return 0;
}

int pthread_cond_signal(pthread_cond_t* cond) {
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t* cond) {
    return 0;
}

// No thread could signal: the wait would never end.
int pthread_cond_wait(pthread_cond_t* restrict cond, pthread_mutex_t* restrict mutex) {
    static const char message[] = "pthread_cond_wait: no other thread to signal\n";
    write(2, message, sizeof message - 1);
    abort();
}

int pthread_cond_timedwait(pthread_cond_t* restrict cond, pthread_mutex_t* restrict mutex, const struct timespec* restrict at) {
    pthread_mutex_unlock(mutex);
    sleepUntil(at);
    pthread_mutex_lock(mutex);

    return ETIMEDOUT;
}

int pthread_rwlock_init(pthread_rwlock_t* restrict lock, const pthread_rwlockattr_t* restrict attr) {
    lock->__readers = 0;
    lock->__writer = 0;

    return 0;
}

int pthread_rwlock_destroy(pthread_rwlock_t* lock) {
    return lock->__readers || lock->__writer ? EBUSY : 0;
}

int pthread_rwlock_tryrdlock(pthread_rwlock_t* lock) {
    if (lock->__writer) {
        return EBUSY;
    }

    lock->__readers++;

    return 0;
}

int pthread_rwlock_rdlock(pthread_rwlock_t* lock) {
    return pthread_rwlock_tryrdlock(lock) == EBUSY ? EDEADLK : 0;
}

int pthread_rwlock_trywrlock(pthread_rwlock_t* lock) {
    if (lock->__writer || lock->__readers) {
        return EBUSY;
    }

    lock->__writer = 1;

    return 0;
}

int pthread_rwlock_wrlock(pthread_rwlock_t* lock) {
    return pthread_rwlock_trywrlock(lock) == EBUSY ? EDEADLK : 0;
}

int pthread_rwlock_unlock(pthread_rwlock_t* lock) {
    if (lock->__writer) {
        lock->__writer = 0;
    } else if (lock->__readers) {
        lock->__readers--;
    } else {
        return EPERM;
    }

    return 0;
}
