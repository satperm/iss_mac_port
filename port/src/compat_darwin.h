/* macOS stand-ins for the Linux APIs the runtime uses (as compat_win.h is for Windows),
 * after bloodborne_mac_port's runtime port. Included by runtime.h on __APPLE__ only.
 * Timed lock waits retry until the realtime deadline (macOS has no timed mutex or rwlock
 * acquire): up to ~1 ms late, like bloodborne_mac_port. */
#ifndef TLG_COMPAT_DARWIN_H
#define TLG_COMPAT_DARWIN_H
#ifdef __APPLE__

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* gettid(): the Mach thread id, as the GPU library's (gpu/shim/tlg_threads.h), so thread ids
 * the runtime and libtlggpu report agree. */
static inline pid_t gettid(void) {
    uint64_t id = 0;
    pthread_threadid_np(NULL, &id);
    return (pid_t)id;
}

#ifndef CLOCK_REALTIME_COARSE
#define CLOCK_REALTIME_COARSE CLOCK_REALTIME
#endif
#ifndef CLOCK_MONOTONIC_COARSE
#define CLOCK_MONOTONIC_COARSE CLOCK_MONOTONIC
#endif

/* struct stat: Linux's st_*tim are st_*timespec on macOS. */
#define st_atim st_atimespec
#define st_mtim st_mtimespec
#define st_ctim st_ctimespec

/* getrandom(2): arc4random_buf never fails or blocks. */
static inline ssize_t getrandom(void *buffer, size_t length, unsigned flags) {
    (void)flags;
    arc4random_buf(buffer, length);
    return (ssize_t)length;
}

/* glibc's name for the recursive initializer. */
#ifndef PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP
#define PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP PTHREAD_RECURSIVE_MUTEX_INITIALIZER
#endif

/* clock_nanosleep(2) is missing: sleeps until an absolute time on `clock` (TIMER_ABSTIME) or
 * for a relative time, through nanosleep. Returns 0 or the error, like the Linux call. */
#ifndef TIMER_ABSTIME
#define TIMER_ABSTIME 1
#endif
static inline int clock_nanosleep(clockid_t clock, int flags, const struct timespec *request,
                                  struct timespec *remain) {
    struct timespec wait = *request;
    if (flags & TIMER_ABSTIME) {
        struct timespec now;
        if (clock_gettime(clock, &now)) return errno;
        int64_t left = ((int64_t)request->tv_sec - (int64_t)now.tv_sec) * 1000000000ll +
                       (request->tv_nsec - now.tv_nsec);
        if (left <= 0) return 0;
        wait.tv_sec = (time_t)(left / 1000000000ll);
        wait.tv_nsec = (long)(left % 1000000000ll);
    }
    return nanosleep(&wait, remain) ? errno : 0;
}

/* POSIX barriers (optional in POSIX, absent on macOS): mutex, condition and a generation count. */
#ifndef PTHREAD_BARRIER_SERIAL_THREAD
#define PTHREAD_BARRIER_SERIAL_THREAD (-1)
typedef int pthread_barrierattr_t;
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    unsigned count, waiting, generation;
} pthread_barrier_t;
static inline int pthread_barrier_init(pthread_barrier_t *b, const pthread_barrierattr_t *attr, unsigned count) {
    (void)attr;
    if (!count) return EINVAL;
    b->count = count; b->waiting = 0; b->generation = 0;
    int e = pthread_mutex_init(&b->mutex, NULL);
    if (e) return e;
    e = pthread_cond_init(&b->cond, NULL);
    if (e) pthread_mutex_destroy(&b->mutex);
    return e;
}
static inline int pthread_barrier_destroy(pthread_barrier_t *b) {
    pthread_cond_destroy(&b->cond);
    return pthread_mutex_destroy(&b->mutex);
}
static inline int pthread_barrier_wait(pthread_barrier_t *b) {
    pthread_mutex_lock(&b->mutex);
    const unsigned generation = b->generation;
    if (++b->waiting == b->count) {
        b->waiting = 0; ++b->generation;
        pthread_cond_broadcast(&b->cond);
        pthread_mutex_unlock(&b->mutex);
        return PTHREAD_BARRIER_SERIAL_THREAD;
    }
    while (generation == b->generation) pthread_cond_wait(&b->cond, &b->mutex);
    pthread_mutex_unlock(&b->mutex);
    return 0;
}
#endif

/* runtime_freeze.c: whether a thread with this Mach thread id still exists. */
int runtime_darwin_thread_alive(uint64_t tid);

/* No per-CPU id for user threads on macOS. */
static inline int sched_getcpu(void) { return 0; }

static inline uint64_t tlg_darwin_realtime_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

/* Waits for the next retry of a timed acquire: yield first, then 50 us, then 1 ms. */
static inline void tlg_darwin_backoff(unsigned attempt) {
    if (attempt < 16) {
        sched_yield();
    } else {
        struct timespec nap = {0, attempt < 64 ? 50000 : 1000000};
        nanosleep(&nap, NULL);
    }
}

static inline uint64_t tlg_darwin_deadline_ns(const struct timespec *abstime) {
    return (uint64_t)abstime->tv_sec * 1000000000ull + (uint64_t)abstime->tv_nsec;
}

static inline int pthread_mutex_timedlock(pthread_mutex_t *mutex, const struct timespec *abstime) {
    const uint64_t deadline = tlg_darwin_deadline_ns(abstime);
    for (unsigned attempt = 0;; ++attempt) {
        int e = pthread_mutex_trylock(mutex);
        if (e != EBUSY) return e;
        if (tlg_darwin_realtime_ns() >= deadline) return ETIMEDOUT;
        tlg_darwin_backoff(attempt);
    }
}

static inline int pthread_rwlock_timedrdlock(pthread_rwlock_t *lock, const struct timespec *abstime) {
    const uint64_t deadline = tlg_darwin_deadline_ns(abstime);
    for (unsigned attempt = 0;; ++attempt) {
        int e = pthread_rwlock_tryrdlock(lock);
        if (e != EBUSY) return e;
        if (tlg_darwin_realtime_ns() >= deadline) return ETIMEDOUT;
        tlg_darwin_backoff(attempt);
    }
}

static inline int pthread_rwlock_timedwrlock(pthread_rwlock_t *lock, const struct timespec *abstime) {
    const uint64_t deadline = tlg_darwin_deadline_ns(abstime);
    for (unsigned attempt = 0;; ++attempt) {
        int e = pthread_rwlock_trywrlock(lock);
        if (e != EBUSY) return e;
        if (tlg_darwin_realtime_ns() >= deadline) return ETIMEDOUT;
        tlg_darwin_backoff(attempt);
    }
}

#endif /* __APPLE__ */
#endif
