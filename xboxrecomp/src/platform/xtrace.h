/*
 * xtrace.h -- timeline tracing for Perfetto (ui.perfetto.dev).
 *
 * The interface follows LVGL's profiler (lv_profiler.h): a scope is a pair
 * of macros, named after the function unless a tag is given --
 *
 *     XTRACE_BEGIN;                    ...  XTRACE_END;
 *     XTRACE_BEGIN_TAG("GIL wait");    ...  XTRACE_END_TAG("GIL wait");
 *
 * -- and each subsystem has its own set, which compiles to nothing unless
 * that subsystem is enabled (XTRACE_DRAW_*: the per-draw phases, sampled 1
 * draw in 16; XTRACE_KERNEL_*: every kernel call). Build with
 * -DXTRACE_DISABLE to compile all of it out.
 *
 * Unlike LVGL's built-in profiler, each thread records into a buffer of its
 * own (no lock), the output is a native Perfetto protobuf trace (.pftrace,
 * ~20 bytes an event) and recording is switched at run time:
 *
 *   RECOMP_TRACE=<path>       arm it (nothing is recorded yet)
 *   kill -USR2 <pid>          start; again to stop and write <path> (written
 *                             as <path>.tmp and renamed). r36s/perfetto_bridge.py
 *                             does this from Perfetto UI's Record button.
 *   RECOMP_TRACE_SECS=<n>     stop by itself n s after starting (default 10;
 *                             0 = only on the second USR2)
 *   RECOMP_TRACE_START=1      start at boot
 *   RECOMP_TRACE_EVENTS=<n>   events a thread (default 256k = 4 MB)
 *   RECOMP_TRACE_DRAW=<n>     split 1 draw in n into phases (default 16; 1 =
 *                             every draw -- ~7 clock reads a draw, a few ms a
 *                             frame on the GL thread: for looking, not timing)
 *
 * Not recording, a scope is one load and a branch. Tags must be string
 * literals or other static strings: the pointer is the key.
 */
#ifndef XTRACE_H
#define XTRACE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern volatile int g_xtrace_on;

/* 'B' begin, 'E' end, 'I' instant (tags as in LVGL / ftrace markers). */
void xtrace_write(const char *tag, char type);
void xtrace_write_at(const char *tag, char type, uint64_t ts_ns);
void xtrace_counter_write(const char *name, int64_t value);
uint64_t xtrace_now(void);                  /* CLOCK_MONOTONIC ns, the trace's clock */
void xtrace_thread_name(const char *name);   /* this thread's track name (static) */
void xtrace_init(void);                      /* reads RECOMP_TRACE; idempotent */

#ifndef XTRACE_DISABLE
#define XTRACE_ON                   (g_xtrace_on)
#define XTRACE_BEGIN_TAG(tag)       do { if (g_xtrace_on) xtrace_write((tag), 'B'); } while (0)
#define XTRACE_END_TAG(tag)         do { if (g_xtrace_on) xtrace_write((tag), 'E'); } while (0)
#define XTRACE_INSTANT(tag)         do { if (g_xtrace_on) xtrace_write((tag), 'I'); } while (0)
#define XTRACE_COUNTER(name, v)     do { if (g_xtrace_on) xtrace_counter_write((name), (int64_t)(v)); } while (0)
/* A span measured by the caller, kept only when it turned out to matter. */
#define XTRACE_SPAN(tag, t0, t1)    do { if (g_xtrace_on) { xtrace_write_at((tag), 'B', (t0)); \
                                         xtrace_write_at((tag), 'E', (t1)); } } while (0)
#else
#define XTRACE_ON                   0
#define XTRACE_BEGIN_TAG(tag)       ((void)(tag))
#define XTRACE_END_TAG(tag)         ((void)(tag))
#define XTRACE_INSTANT(tag)         ((void)(tag))
#define XTRACE_COUNTER(name, v)     ((void)(name), (void)(v))
#define XTRACE_SPAN(tag, t0, t1)    ((void)(tag), (void)(t0), (void)(t1))
#endif
#define XTRACE_BEGIN                XTRACE_BEGIN_TAG(__func__)
#define XTRACE_END                  XTRACE_END_TAG(__func__)

/* Per-draw phases (nv2a_gl.c): one draw in RECOMP_TRACE_DRAW (default
 * XTRACE_DRAW_SAMPLE), each phase a span from the previous mark. The others
 * are only the outer "draw" span. -DXTRACE_DRAW=0 compiles them out. */
#ifndef XTRACE_DRAW
#define XTRACE_DRAW 1
#endif
#define XTRACE_DRAW_SAMPLE 16
#if XTRACE_DRAW && !defined(XTRACE_DISABLE)
extern __thread int g_xtrace_draw_on;
extern __thread uint64_t g_xtrace_draw_t;
void xtrace_draw_start(void);
#define XTRACE_DRAW_START           do { if (g_xtrace_on) xtrace_draw_start(); \
                                         else g_xtrace_draw_on = 0; } while (0)
#define XTRACE_DRAW_MARK(tag)       do { if (g_xtrace_draw_on) { uint64_t t_ = xtrace_now(); \
                                         xtrace_write_at((tag), 'B', g_xtrace_draw_t); \
                                         xtrace_write_at((tag), 'E', t_); \
                                         g_xtrace_draw_t = t_; } } while (0)
#define XTRACE_DRAW_COUNTER(n, v)   do { if (g_xtrace_draw_on) xtrace_counter_write((n), (int64_t)(v)); } while (0)
#else
#define XTRACE_DRAW_START           ((void)0)
#define XTRACE_DRAW_MARK(tag)       ((void)(tag))
#define XTRACE_DRAW_COUNTER(n, v)   ((void)(n), (void)(v))
#endif

#ifdef __cplusplus
}
#endif
#endif /* XTRACE_H */
