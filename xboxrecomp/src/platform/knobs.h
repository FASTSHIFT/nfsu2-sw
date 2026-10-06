/* knobs -- per-second wake counters and live-tunable integers.
 *
 * Diagnosis without restarting the game: hot paths bump a counter
 * (KNOB_COUNT), a background thread prints the per-second rates
 * ("[wake] ...", also as xtrace counters), and re-reads a KEY=VALUE file
 * (RECOMP_KNOBS, default /tmp/nfsu2.knobs) every second so a tunable takes
 * effect in a running game. Every tunable's default is the stock behaviour.
 *
 * RECOMP_KNOBS=0 turns the thread off (counters still count, nothing is
 * printed, tunables stay at their defaults). */
#ifndef KNOBS_H
#define KNOBS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Counters: what woke or yielded, per second. */
enum {
    KC_SPIN_WAKE,       /* recomp_spin_wake calls */
    KC_SPIN_BCAST,      /*   ... that broadcast to a sleeping hardware thread */
    KC_HW_SLEEP,        /* nv2a_thread_pause sleeps (flag + executor threads) */
    KC_HW_TIMEOUT,      /*   ... that ran out the timeout instead of being woken */
    KC_HW_YIELD,        /* nv2a_thread_pause busy yields */
    KC_SPIN_YIELD,      /* recomp_spin_yield calls */
    KC_SPIN_YIELD_REAL, /*   ... that gave up the GIL and sched_yield'ed */
    KC_GIL_WAIT,        /* gil_lock calls that had to wait */
    KC_GIL_TIMEOUT,     /*   1 ms timeouts while waiting */
    KC_KDISP_SLEEP,     /* kdisp_wait condition sleeps (KeWait*) */
    KC_UP_VTX_KB,       /* GL uploads, KB: vertex ring */
    KC_UP_IDX_KB,       /*   index ring */
    KC_UP_TEX_KB,       /*   textures (decoded or straight from guest memory) */
    KC_UP_TEX_N,        /*   texture uploads */
    KC_COUNT
};

/* Tunables. */
enum {
    KN_SPIN_WAKE_EVERY, /* spin_wake broadcasts at most once per n calls (1) */
    KN_SPIN_YIELD_EVERY,/* spin-hint yields really yield once per n calls (1) */
    KN_SPIN_YIELD_IDLE, /* 1: skip the yield when nobody waits for the GIL (0) */
    KN_HW_SLEEP_US,     /* hardware-thread sleep timeout, us (1000) */
    KN_GIL_WAIT_MS,     /* GIL waiter timeout, ms (1) */
    KN_FPS_CAP,         /* sleep before present to at most n fps; 0 = off (0) */
    KN_COUNT
};

extern volatile unsigned g_knob_count[KC_COUNT];
extern volatile int g_knob[KN_COUNT];

#define KNOB_COUNT(id) __atomic_add_fetch(&g_knob_count[id], 1u, __ATOMIC_RELAXED)
#define knob(id)       (g_knob[id])

void knobs_init(void);              /* idempotent; starts the thread */

#ifdef __cplusplus
}
#endif
#endif
