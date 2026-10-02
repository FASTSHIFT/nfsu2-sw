/* Windows stubs for the game-specific thread helpers that live in
 * win32_compat.c on POSIX (that file is Linux-only). The real work is
 * Switch-specific; on Windows these are no-ops: correctness does not depend
 * on thread pinning or priority, only performance tuning does. */
#include <stddef.h>

void xbox_guest_pin(int interrupt) { (void)interrupt; }
void xbox_nx_raise_host_thread(void) { }
void xbox_nx_retag_thread(void *entry) { (void)entry; }
