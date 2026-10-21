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
    KN_POKE_VA,         /* guest VA to hold at KN_POKE_VAL every second (0) */
    KN_POKE_VAL,        /* value written to KN_POKE_VA (0) */
    KN_DUMP_DRAWS,      /* 1: print this frame's draw census at the next flip (0) */
    KN_HUD,             /* 1: on-screen perf HUD (same as RECOMP_HUD=1) (0) */
    KN_EXP_DROPFRAG,    /* experiment: 1 = skip draws under 50 indices, 2 = skip all (0) */
    KN_EXP_RD,          /* experiment: 2 = GL_RASTERIZER_DISCARD around draws, 1 = control (0) */
    KN_EXP_QUERY,       /* experiment: 1 = count this frame's draws that put no pixel on screen, once (0) */
    KN_EXP_STATIC,      /* experiment: 1 = log how many draws repeat last frame's mesh + matrix rows (0) */
    KN_EXP_RDSEL,       /* ... only for: 1 blended, 2 opaque, 3 < 50 indices, 4 >= 50 (0 = all) */
    KN_GL_LEGACY,       /* bit 0: vertex attributes by glVertexAttribPointer, not vertex_attrib_binding (0) */
    KN_DUMP_SEQ,        /* a new nonzero value dumps the next presented frame to /tmp/glab_<n>.bmp (0) */
    KN_EXP_NOUNI,       /* experiment, TIMING ONLY (picture breaks): 1 = scan the c[] rows but skip their glUniform4fv (0) */
    KN_CSCAN,           /* 1 = c[] rows found by memcmp against the program's copy, not by the GL thread's row stamps (0) */
    KN_PX_PCT,          /* scenery on-screen size is scaled by this % before the draw-distance tests (100 = stock; 50 = objects must be twice as big to be drawn) */
    KN_PX_BIG,          /* PX_PCT leaves instances whose bounding radius (+6) is at least this many units alone: the sky dome, landmark buildings (0 = scale all) */
    KN_PX_STAT,         /* 1: log a histogram of the radii GetPixelSize sees, once (0) */
    KN_NATIVE_OFF,      /* unused since the upstream merge (RECOMP_NATIVE=0 / RECOMP_NATIVE_CHECK=1 instead); kept so knob numbering stays (0) */
    KN_EXP_MERGESTAT,   /* experiment: 1 = log per frame how many draws are pre-transformed and how many runs of them share program/textures/surface/state (0) */
    KN_RAMP_EPS,        /* contrast ramp (sub_000A3FA0) rebuilt only when a parameter moved more than this x 1e-4 since the last build (20; 0 = stock: any change) */
    KN_CLR_DEDUP,       /* 1: drop a clear that repeats the last write to its surface and depth buffer (a pass whose draws RECOMP_GL_SKIP drops) (1) */
    KN_TEX_MEMO,        /* 1: a texture stage that samples what it did on the previous draw skips the surface search and tex_get (1) */
    KN_GL_MERGE,        /* 1: a triangle-list draw that needs no GL state change after the previous one joins its glDrawRangeElements (0: the R36S hung twice with it on, docs/13 §12) */
    KN_PHYS_PCT,        /* NFSU2: PhysicsMover's longest physics substep (MaximumTimeStep, 0.0044 s) in % of stock; 150 = a third fewer substeps (100) */
    KN_MELLOW_PCT,      /* NFSU2: the same for MellowMover (far/traffic cars, MaxiMellowTimeStep 1/70 s) (100) */
    KN_CAR_LOD,         /* NFSU2: ForceCarLOD, every car body at this model LOD, 0 (most detail) .. 3; -1 = by size on screen (-1) */
    KN_TIRE_LOD,        /* NFSU2: ForceTireLOD, the same for wheels (-1) */
    KN_CAR_LOD_PCT,     /* NFSU2: the on-screen sizes at which car bodies switch LOD (Car/TrafficCarBodyLodSwapSize) in %; 200 = lower LODs twice as near (100) */
    KN_COUNT
};

/* Optional, defined by the game project: called by the knobs thread after
 * every re-read of the file (once a second), to apply game-specific knobs. */
extern void knobs_game_apply(void) __attribute__((weak));

extern volatile unsigned g_knob_count[KC_COUNT];
extern volatile int g_knob[KN_COUNT];

#define KNOB_COUNT(id) __atomic_add_fetch(&g_knob_count[id], 1u, __ATOMIC_RELAXED)
#define knob(id)       (g_knob[id])
#define knob_set(id, v) __atomic_store_n(&g_knob[id], (v), __ATOMIC_RELAXED)

void knobs_init(void);              /* idempotent; starts the thread */

#ifdef __cplusplus
}
#endif
#endif
