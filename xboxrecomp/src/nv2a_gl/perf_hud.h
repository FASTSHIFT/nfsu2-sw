/* perf_hud -- on-screen frame stats, drawn into the finished frame.
 *
 * Ported from reVC's HUD (re3-sdl/src/extras/perfhud*.cpp): the same lines
 * (FPS, frame ms, CPU/GPU split, draw calls, system load, memory, clocks,
 * temperature) and the same left-middle position, so a number on the device
 * can be read without RenderDoc or a trace.
 *
 * Off by default. RECOMP_HUD=1, or the knob HUD=1 at runtime. Drawn from the
 * GL thread right before the frame is handed to the display, into the 640x480
 * surface, so it costs one program switch and ~1 draw, and sits on top of
 * everything the title rendered.
 *
 * Besides the HUD, every 120 frames one "[perf]" line goes to stderr with the
 * window averages, min/max frame time and the extra NFSU2 columns, whether or
 * not the HUD is shown (RECOMP_PERF_LOG=1). RECOMP_PERF_LOG=2 prints one
 * "[frame]" line per frame instead, for before/after comparisons.
 */
#ifndef NV2A_PERF_HUD_H
#define NV2A_PERF_HUD_H

#include <stdint.h>

typedef struct {
    double frame_ms;     /* wall time between two presents */
    double gl_ms;        /* GL thread busy: frame - swap - wait */
    double wait_ms;      /* GL thread starved, waiting for the game thread */
    double swap_ms;      /* time blocked in present (SwapWindow / KMS wait) */
    unsigned draws;      /* GL draw calls this frame */
    unsigned frag;       /* ... of them with fewer than 50 indices (the merge candidates) */
    unsigned gl_calls;   /* all GL calls this frame */
    unsigned merged;     /* sub-draws folded into batches (RECOMP_GL_MERGE) */
} PerfFrame;

/* Called once per frame, before the HUD is drawn. */
void perf_hud_update(const PerfFrame *f);

/* Draw the HUD into the currently bound draw framebuffer, w x h pixels
 * (aa_x/aa_y: the surface holds that many samples per displayed pixel, so
 * glyphs are stretched by it to look square on screen)
 * (top-left origin is row 0 when `top_first`, else bottom-up). No-op if off.
 * Leaves GL state as it found it as far as the renderer's caches go: the
 * caller invalidates them (state_dirty) afterwards. */
void perf_hud_draw(int w, int h, int top_first, float aa_x, float aa_y);

int perf_hud_enabled(void);

#endif
