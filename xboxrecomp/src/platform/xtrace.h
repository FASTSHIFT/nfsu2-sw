/*
 * xtrace.h -- timeline tracing for Perfetto (ui.perfetto.dev).
 *
 * Hand-placed spans on the threads that make up a frame -- where each one
 * waits for another, not every function it runs: the guest lock, kernel
 * waits, the frame fence, the executor, the GL thread's queue and swap, the
 * APU. Written as a native Perfetto protobuf trace (.pftrace, ~10-20 bytes an
 * event), no library needed.
 *
 *   RECOMP_TRACE=<path>       arm it (a buffer per thread, nothing recorded)
 *   kill -USR2 <pid>          start; again to stop and write <path> (each
 *                             recording replaces it; written as <path>.tmp
 *                             and renamed, so a complete file appears at once)
 *   RECOMP_TRACE_SECS=<n>     stop and write by itself n s after starting
 *                             (default 10; 0 = only on the second USR2, as
 *                             r36s/perfetto_bridge.py -- Perfetto UI's Record
 *                             button -- runs it)
 *   RECOMP_TRACE_START=1      start at boot instead of on the first USR2
 *
 * Disarmed, every hook is one load and a branch. Recording, a span is two
 * clock reads and two 16-byte stores into the calling thread's own buffer
 * (no locks); a full buffer drops events and counts them.
 */
#ifndef XTRACE_H
#define XTRACE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Names: static strings only (the pointer is the key). */
extern volatile int g_xtrace_on;

void xtrace_begin_(const char *name);
void xtrace_end_(void);
void xtrace_counter_(const char *name, int64_t value);
void xtrace_instant_(const char *name);
void xtrace_span_(const char *name, uint64_t t0_ns, uint64_t t1_ns);  /* a span after the fact */
uint64_t xtrace_now(void);                    /* CLOCK_MONOTONIC ns, the trace's clock */
void xtrace_thread_name(const char *name);   /* this thread's track name */
void xtrace_init(void);                       /* reads RECOMP_TRACE; idempotent */

#define xtrace_begin(n)      do { if (g_xtrace_on) xtrace_begin_(n); } while (0)
#define xtrace_end()         do { if (g_xtrace_on) xtrace_end_(); } while (0)
#define xtrace_counter(n, v) do { if (g_xtrace_on) xtrace_counter_(n, (int64_t)(v)); } while (0)
#define xtrace_instant(n)    do { if (g_xtrace_on) xtrace_instant_(n); } while (0)
/* A span recorded after it happened (only kept when it turned out to matter). */
#define xtrace_span(n, t0, t1) do { if (g_xtrace_on) xtrace_span_(n, t0, t1); } while (0)

#ifdef __cplusplus
}
#endif
#endif /* XTRACE_H */
