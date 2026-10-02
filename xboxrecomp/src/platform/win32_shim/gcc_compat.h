/* gcc_compat.h -- minimal GCC-isms for MSVC, used by the NV2A OpenGL renderer.
 *
 * The renderer is written for GCC/Linux/Switch and uses __attribute__((weak))
 * and the __atomic_* builtins. MSVC has neither. The atomics here are plain
 * loads/stores: they guard the opt-in RECOMP_GL_THREAD queue, which Windows
 * does not enable, so relaxed single-threaded access is correct for now.
 */
#ifndef WIN32_GCC_COMPAT_H
#define WIN32_GCC_COMPAT_H

#define __attribute__(x)

/* clock_gettime clock ids, defined here because nv2a_gl.c uses CLOCK_MONOTONIC
 * long before the pthread.h shim (which also declares clock_gettime) is
 * included near the bottom of the file. */
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 0
#endif
#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME  1
#endif

#define __ATOMIC_RELAXED 0
#define __ATOMIC_ACQUIRE 0
#define __ATOMIC_RELEASE 0
#define __ATOMIC_ACQ_REL 0
#define __ATOMIC_SEQ_CST 0

#define __atomic_load_n(p, o)        (*(p))
#define __atomic_store_n(p, v, o)    ((*(p)) = (v))
#define __atomic_add_fetch(p, v, o)  ((*(p)) += (v))
#define __atomic_sub_fetch(p, v, o)  ((*(p)) -= (v))

static __inline unsigned long long _gcc_xchg64(unsigned long long *p,
                                               unsigned long long v)
{
    unsigned long long o = *p;
    *p = v;
    return o;
}
#define __atomic_exchange_n(p, v, o) (_gcc_xchg64((unsigned long long *)(p), (v)))

#endif
