/* gl_vsh.h -- NV2A vertex transform as GLSL (see gl_vsh.c). */
#ifndef NV2A_GL_VSH_H
#define NV2A_GL_VSH_H

#include <stddef.h>
#include <stdint.h>

/* The body of a vertex program, as `void nv2a_program(void)`, from slot
 * `start` to the FINAL instruction. Returns bytes written, -1 on overflow. */
int nv2a_gl_vsh_program(const uint32_t (*prog)[4], uint32_t slots,
                        uint32_t start, char *buf, size_t cap);

/* Nonzero: emit Vulkan GLSL (nv2a_vk). Set once, before the first shader. */
extern int nv2a_shader_vk;
/* Nonzero: emit GLSL ES 3.00 (the GL renderer on an OpenGL ES context). */
extern int nv2a_shader_es;

/* Nonzero: the vertex shader reads its constants from a per-draw block
 * (`c[blk*192 + i]`, blk from gl_VertexID), so several draws that share a
 * program can be merged into one. Desktop GL only (the enlarged uniform
 * array exceeds GLES 3.0's 256-vector minimum). Set once, before the first
 * shader. */
extern int nv2a_shader_merge;
/* Draws per merged batch: the cap (NV2A_MERGE_MAX) and the runtime value the
 * shader is generated with, set from GL_MAX_VERTEX_UNIFORM_VECTORS before the
 * first shader compiles (Mali/GLES allows fewer blocks than desktop GL). */
extern int nv2a_merge_max;
#define NV2A_MERGE_MAX 16

/* Declarations and helpers every vertex shader starts with. */
const char *nv2a_gl_vsh_prelude(void);
/* Follows main(): "" on desktop GL; on ES the real main, which fixes up the
 * inputs (BGRA colours) and calls the renamed one. */
const char *nv2a_gl_vsh_epilogue(void);
/* main() for a vertex-program shader (follows the program body). */
const char *nv2a_gl_vsh_main_program(void);
/* main() for fixed-function / pre-transformed batches (no program body). */
const char *nv2a_gl_vsh_fixed(void);

#endif
