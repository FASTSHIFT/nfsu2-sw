/* knobs.c -- see knobs.h. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "knobs.h"
#include "xtrace.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

volatile unsigned g_knob_count[KC_COUNT];
volatile int g_knob[KN_COUNT] = {
    [KN_SPIN_WAKE_EVERY]  = 1,
    [KN_SPIN_YIELD_EVERY] = 1,
    [KN_SPIN_YIELD_IDLE]  = 0,
    [KN_HW_SLEEP_US]      = 1000,
    [KN_GIL_WAIT_MS]      = 1,
    [KN_FPS_CAP]          = 0,
};

static const char *const s_count_name[KC_COUNT] = {
    "spin_wake", "bcast", "hw_sleep", "hw_tmo", "hw_yield",
    "yield", "yield_real", "gil_wait", "gil_tmo", "kdisp",
    "up_vtx_kb", "up_idx_kb", "up_tex_kb", "up_tex_n",
};
static const char *const s_knob_name[KN_COUNT] = {
    "SPIN_WAKE_EVERY", "SPIN_YIELD_EVERY", "SPIN_YIELD_IDLE",
    "HW_SLEEP_US", "GIL_WAIT_MS", "FPS_CAP",
};
static const int s_knob_min[KN_COUNT] = { 1, 1, 0, 50, 1, 0 };

static int s_quiet;                  /* RECOMP_KNOBS_QUIET: no [wake] lines */

static void knobs_reload(const char *path)
{
    static int def[KN_COUNT], have_def;
    int want[KN_COUNT];
    char line[128];
    FILE *f;
    int k;

    if (!have_def) {
        /* RECOMP_KNOB_<NAME>=v overrides the built-in default; the file
         * overrides that, and a key removed from the file reverts to it. */
        for (k = 0; k < KN_COUNT; k++) {
            char env[64];
            const char *e;
            snprintf(env, sizeof env, "RECOMP_KNOB_%s", s_knob_name[k]);
            if ((e = getenv(env)) != NULL && *e) {
                int v = atoi(e);
                g_knob[k] = v < s_knob_min[k] ? s_knob_min[k] : v;
                fprintf(stderr, "[knob] %s = %d (%s)\n", s_knob_name[k], g_knob[k], env);
            }
        }
        memcpy(def, (const void *)g_knob, sizeof def);
        have_def = 1;
    }
    memcpy(want, def, sizeof want);      /* a key removed from the file reverts */
    f = *path ? fopen(path, "r") : NULL;
    if (f) {
        while (fgets(line, sizeof line, f)) {
            char *eq = strchr(line, '=');
            if (!eq || line[0] == '#')
                continue;
            *eq = 0;
            for (k = 0; k < KN_COUNT; k++)
                if (strcmp(line, s_knob_name[k]) == 0) {
                    int v = atoi(eq + 1);
                    want[k] = v < s_knob_min[k] ? s_knob_min[k] : v;
                }
        }
        fclose(f);
    }
    for (k = 0; k < KN_COUNT; k++)
        if (want[k] != g_knob[k]) {
            fprintf(stderr, "[knob] %s %d -> %d\n", s_knob_name[k], g_knob[k], want[k]);
            g_knob[k] = want[k];
        }
}

static void *knobs_thread(void *arg)
{
    const char *path = (const char *)arg;
    unsigned prev[KC_COUNT] = { 0 };
    char out[512];
    int k;

    xtrace_thread_name("knobs");
    for (;;) {
        size_t n = 0;
        sleep(1);
        knobs_reload(path);
        if (s_quiet)
            continue;
        n += (size_t)snprintf(out + n, sizeof out - n, "[wake]");
        for (k = 0; k < KC_COUNT; k++) {
            unsigned now = __atomic_load_n(&g_knob_count[k], __ATOMIC_RELAXED);
            unsigned d = now - prev[k];
            prev[k] = now;
            n += (size_t)snprintf(out + n, sizeof out - n, " %s=%u", s_count_name[k], d);
            XTRACE_COUNTER(s_count_name[k], d);
            if (n >= sizeof out)
                n = sizeof out - 1;
        }
        fprintf(stderr, "%s\n", out);
        fflush(stderr);
    }
    return NULL;
}

void knobs_init(void)
{
    static int done;
    const char *e = getenv("RECOMP_KNOBS");
    const char *path = e && *e ? e : "/tmp/nfsu2.knobs";
    pthread_t th;

    if (done++)
        return;
    if (e && strcmp(e, "0") == 0) {
        knobs_reload("");                /* env defaults only, no file, no thread */
        return;
    }
    knobs_reload(path);                  /* env defaults + file, before the title runs */
    s_quiet = getenv("RECOMP_KNOBS_QUIET") != NULL;
    if (pthread_create(&th, NULL, knobs_thread, (void *)path) == 0)
        pthread_detach(th);
}
