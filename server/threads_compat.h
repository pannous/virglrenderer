/* threads.h compatibility for macOS using pthreads */
#ifndef THREADS_COMPAT_H
#define THREADS_COMPAT_H

#include <pthread.h>
#include <errno.h>
#include <stdlib.h>
#include <stdint.h>

/* Thread types */
typedef pthread_t thrd_t;
typedef int (*thrd_start_t)(void*);

/* Thread return values */
enum {
    thrd_success = 0,
    thrd_error = 1,
    thrd_busy = 2,
    thrd_nomem = 3,
    thrd_timedout = 4
};

/* Wrapper struct for start function */
struct thrd_wrapper_data {
    thrd_start_t func;
    void *arg;
};

static inline void *thrd_wrapper(void *arg) {
    struct thrd_wrapper_data *data = (struct thrd_wrapper_data *)arg;
    thrd_start_t func = data->func;
    void *func_arg = data->arg;
    free(data);
    int ret = func(func_arg);
    return (void *)(intptr_t)ret;
}

static inline int thrd_create(thrd_t *thr, thrd_start_t func, void *arg) {
    struct thrd_wrapper_data *data = malloc(sizeof(*data));
    if (!data) return thrd_nomem;
    data->func = func;
    data->arg = arg;
    int ret = pthread_create(thr, NULL, thrd_wrapper, data);
    if (ret != 0) {
        free(data);
        return thrd_error;
    }
    return thrd_success;
}

static inline int thrd_join(thrd_t thr, int *res) {
    void *ret;
    int err = pthread_join(thr, &ret);
    if (err != 0) return thrd_error;
    if (res) *res = (int)(intptr_t)ret;
    return thrd_success;
}

static inline thrd_t thrd_current(void) {
    return pthread_self();
}

static inline int thrd_equal(thrd_t a, thrd_t b) {
    return pthread_equal(a, b);
}

#endif /* THREADS_COMPAT_H */
