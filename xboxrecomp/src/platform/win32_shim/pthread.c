#include "pthread.h"

/* ---- clock_gettime ---- */

int clock_gettime(clockid_t clk_id, struct timespec *tp)
{
    static LARGE_INTEGER freq;
    static int have_freq;

    if (!have_freq) {
        QueryPerformanceFrequency(&freq);
        have_freq = 1;
    }

    if (clk_id == CLOCK_REALTIME) {
        FILETIME ft;
        ULARGE_INTEGER u;
        GetSystemTimeAsFileTime(&ft);
        u.LowPart = ft.dwLowDateTime;
        u.HighPart = ft.dwHighDateTime;
        u.QuadPart -= 116444736000000000ULL;   /* 1601 -> 1970, 100 ns */
        tp->tv_sec = (time_t)(u.QuadPart / 10000000ULL);
        tp->tv_nsec = (long)((u.QuadPart % 10000000ULL) * 100);
        return 0;
    }

    {
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        tp->tv_sec = (time_t)(c.QuadPart / freq.QuadPart);
        tp->tv_nsec = (long)((c.QuadPart % freq.QuadPart) * 1000000000ULL
                             / freq.QuadPart);
    }
    return 0;
}

/* ---- mutex ---- */

int pthread_mutex_init(pthread_mutex_t *m, const void *a)
{ (void)a; InitializeSRWLock(m); return 0; }
int pthread_mutex_lock(pthread_mutex_t *m)
{ AcquireSRWLockExclusive(m); return 0; }
int pthread_mutex_unlock(pthread_mutex_t *m)
{ ReleaseSRWLockExclusive(m); return 0; }
int pthread_mutex_trylock(pthread_mutex_t *m)
{ return TryAcquireSRWLockExclusive(m) ? 0 : 1; }
int pthread_mutex_destroy(pthread_mutex_t *m)
{ (void)m; return 0; }

/* ---- cond ---- */

int pthread_cond_init(pthread_cond_t *c, const void *a)
{ (void)a; InitializeConditionVariable(c); return 0; }
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m)
{ return SleepConditionVariableSRW(c, m, INFINITE, 0) ? 0 : -1; }
int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m,
                           const struct timespec *abstime)
{
    struct timespec now;
    long long ms;
    clock_gettime(CLOCK_REALTIME, &now);
    ms = ((long long)abstime->tv_sec - (long long)now.tv_sec) * 1000
       + ((long long)abstime->tv_nsec - (long long)now.tv_nsec) / 1000000;
    if (ms < 0) ms = 0;
    if (ms > 0x7FFFFFFFLL) ms = 0x7FFFFFFFLL;
    return SleepConditionVariableSRW(c, m, (DWORD)ms, 0) ? 0 : 1;
}
int pthread_cond_broadcast(pthread_cond_t *c)
{ WakeAllConditionVariable(c); return 0; }
int pthread_cond_signal(pthread_cond_t *c)
{ WakeConditionVariable(c); return 0; }
int pthread_cond_destroy(pthread_cond_t *c)
{ (void)c; return 0; }

/* ---- threads ---- */

int pthread_attr_init(pthread_attr_t *a)
{ a->stacksize = 0; return 0; }
int pthread_attr_setstacksize(pthread_attr_t *a, size_t s)
{ a->stacksize = s; return 0; }
int pthread_attr_destroy(pthread_attr_t *a)
{ (void)a; return 0; }

typedef struct { void *(*fn)(void *); void *arg; } pthread_tharg;

static DWORD WINAPI pthread_thrun(LPVOID p)
{
    pthread_tharg *a = (pthread_tharg *)p;
    void *(*fn)(void *) = a->fn;
    void *arg = a->arg;
    free(a);
    fn(arg);
    return 0;
}

int pthread_create(pthread_t *t, const pthread_attr_t *a,
                   void *(*start)(void *), void *arg)
{
    pthread_tharg *ta = (pthread_tharg *)malloc(sizeof(*ta));
    DWORD stack = (a && a->stacksize) ? (DWORD)a->stacksize : 0;
    HANDLE h;
    if (!ta) return -1;
    ta->fn = start;
    ta->arg = arg;
    h = CreateThread(NULL, stack, pthread_thrun, ta, 0, NULL);
    if (!h) { free(ta); return -1; }
    *t = h;
    return 0;
}

int pthread_detach(pthread_t t)
{ CloseHandle(t); return 0; }
