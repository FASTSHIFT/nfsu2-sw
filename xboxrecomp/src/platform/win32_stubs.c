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
void xtrace_begin_(const char *name) { (void)name; }
void xtrace_end_(void) { }
void xtrace_counter_(const char *name, int64_t value) { (void)name; (void)value; }
void xtrace_instant_(const char *name) { (void)name; }
void xtrace_thread_name(const char *name) { (void)name; }
void xtrace_span_(const char *name, uint64_t a, uint64_t b) { (void)name; (void)a; (void)b; }
uint64_t xtrace_now(void) { return 0; }
void xtrace_init(void) { }
