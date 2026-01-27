/* threads.h compatibility for macOS using pthreads */
#ifndef THREADS_COMPAT_H
#define THREADS_COMPAT_H

#include <pthread.h>
#include <errno.h>

typedef pthread_t thrd_t;
typedef int (*thrd_start_t)(void *);

enum {
    thrd_success = 0,
    thrd_error = 1,
    thrd_nomem = 2,
    thrd_timedout = 3,
    thrd_busy = 4
};

static inline int thrd_create(thrd_t *thr, thrd_start_t func, void *arg) {
    int ret = pthread_create(thr, NULL, (void *(*)(void *))func, arg);
    return ret == 0 ? thrd_success : thrd_error;
}

static inline int thrd_join(thrd_t thr, int *res) {
    void *retval;
    int ret = pthread_join(thr, &retval);
    if (res) *res = (int)(intptr_t)retval;
    return ret == 0 ? thrd_success : thrd_error;
}

static inline thrd_t thrd_current(void) {
    return pthread_self();
}

static inline int thrd_equal(thrd_t a, thrd_t b) {
    return pthread_equal(a, b);
}

#endif
