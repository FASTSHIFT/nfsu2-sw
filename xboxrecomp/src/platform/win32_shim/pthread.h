/* pthread.h -- tiny pthread subset for MSVC, enough for the NV2A renderer.
 *
 * Maps pthread mutex/cond/threads onto Windows SRWLOCK, CONDITION_VARIABLE and
 * CreateThread. Both primitives stay statically initialisable, which is what
 * the renderer's PTHREAD_MUTEX_INITIALIZER / PTHREAD_COND_INITIALIZER need.
 */
#ifndef WIN32_PTHREAD_SHIM_H
#define WIN32_PTHREAD_SHIM_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <time.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- clock_gettime ---- */
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 0
#endif
#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME  1
#endif
typedef int clockid_t;
int clock_gettime(clockid_t clk_id, struct timespec *tp);

/* ---- mutex -> SRWLOCK ---- */
typedef SRWLOCK pthread_mutex_t;
#define PTHREAD_MUTEX_INITIALIZER SRWLOCK_INIT
int pthread_mutex_init(pthread_mutex_t *m, const void *a);
int pthread_mutex_lock(pthread_mutex_t *m);
int pthread_mutex_unlock(pthread_mutex_t *m);
int pthread_mutex_trylock(pthread_mutex_t *m);
int pthread_mutex_destroy(pthread_mutex_t *m);

/* ---- cond -> CONDITION_VARIABLE ---- */
typedef CONDITION_VARIABLE pthread_cond_t;
#define PTHREAD_COND_INITIALIZER CONDITION_VARIABLE_INIT
int pthread_cond_init(pthread_cond_t *c, const void *a);
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m);
int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m,
                           const struct timespec *abstime);
int pthread_cond_broadcast(pthread_cond_t *c);
int pthread_cond_signal(pthread_cond_t *c);
int pthread_cond_destroy(pthread_cond_t *c);

/* ---- threads ---- */
typedef HANDLE pthread_t;
typedef struct { size_t stacksize; } pthread_attr_t;
int pthread_attr_init(pthread_attr_t *a);
int pthread_attr_setstacksize(pthread_attr_t *a, size_t size);
int pthread_attr_destroy(pthread_attr_t *a);
int pthread_create(pthread_t *t, const pthread_attr_t *a,
                   void *(*start)(void *), void *arg);
int pthread_detach(pthread_t t);

#ifdef __cplusplus
}
#endif

#endif
