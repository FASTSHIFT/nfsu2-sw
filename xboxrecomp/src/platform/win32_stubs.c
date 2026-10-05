/* Windows stubs for the game-specific thread helpers that live in
 * win32_compat.c on POSIX (that file is Linux-only). The real work is
 * Switch-specific; on Windows these are no-ops: correctness does not depend
 * on thread pinning or priority, only performance tuning does. */
#include <stddef.h>

void xbox_guest_pin(int interrupt) { (void)interrupt; }
void xbox_nx_raise_host_thread(void) { }
void xbox_nx_retag_thread(void *entry) { (void)entry; }
void xbox_nx_spread_thread(void) { }
void xbox_nx_track_thread(void *entry) { (void)entry; }

/* xtrace.h: tracing is POSIX-only (xtrace.c); these keep the hooks linking. */
#include "xtrace.h"
volatile int g_xtrace_on;
void xtrace_write(const char *tag, char type) { (void)tag; (void)type; }
void xtrace_write_at(const char *tag, char type, uint64_t t) { (void)tag; (void)type; (void)t; }
void xtrace_counter_write(const char *name, int64_t value) { (void)name; (void)value; }
uint64_t xtrace_now(void) { return 0; }
void xtrace_thread_name(const char *name) { (void)name; }
void xtrace_init(void) { }
#if XTRACE_DRAW && !defined(XTRACE_DISABLE)
__thread int g_xtrace_draw_on;
__thread uint64_t g_xtrace_draw_t;
void xtrace_draw_start(void) { }
#endif
