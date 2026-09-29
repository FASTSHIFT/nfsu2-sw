/*
 * nv2a_gl.c -- an OpenGL 3.3 core renderer for the pushbuffer executor.
 *
 * Registered as the executor's back end (nv2a_backend.h, draw_raw). The
 * executor decodes the pushbuffer, gathers vertices and keeps the method
 * shadow; this file turns that state into GL:
 *
 *   surfaces   one framebuffer object per colour-surface address, rows
 *              top-first like guest memory. A texture whose address is a
 *              surface samples the FBO directly -- render-to-texture never
 *              round-trips through guest memory.
 *   textures   decoded from guest memory by the executor's own decoder
 *              (swizzled, linear, DXT), cached by address and format, and
 *              re-uploaded when a sampled hash of the bytes changes.
 *   shaders    vertex programs and the fixed-function transform (gl_vsh.c),
 *              register combiners and texture modes (gl_psh.c), linked in
 *              pairs and cached by what generated them.
 *   state      blend, depth, stencil, cull and colour mask straight from the
 *              method shadow; alpha test in the fragment shader.
 *
 * Everything runs on the executor's thread, which is where the GL context is
 * created (lazily, on the first call) and where the window is presented and
 * its events pumped, on flip.
 *
 * Runtime switches:
 *   RECOMP_GL_DUMP=<prefix>[,every]  write presented frames as BMP
 *   RECOMP_GL_TRACE=1                shader sources and link errors
 */
#include "nv2a_gl.h"
#include "gl_api.h"
#include "gl_psh.h"
#include "gl_vsh.h"
#include "../kernel/nv2a_backend.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern ptrdiff_t xbox_GetMemoryOffset(void);

int nv2a_gl_adopt_window(void **win, void **ctx);
int nv2a_gl_draw_placeholder(void);
int nv2a_gl_screen_size(int *w, int *h);
static SDL_Window   *s_win;
static SDL_GLContext s_ctx;
static int           s_state;           /* 0 untried, 1 ready, -1 failed */
static int           s_trace;
static int           s_finish = -1;      /* RECOMP_GL_FINISH */

static int           s_new_obj;          /* this operation uses something new */

/* RECOMP_GL_FINISH: wait for the GPU after every operation, so a hard GPU
 * hang happens at the operation that causes it. Only operations that touch
 * something new (a shader just compiled, a surface or texture just made)
 * are logged -- the rest is thousands of lines a frame, far too slow for a
 * synchronous log on an SD card. */
static void gl_step_done(const char *what)
{
    int upload = what[0] == 't';          /* "texture upload": part of a draw */
    if (s_finish < 0)
        s_finish = getenv("RECOMP_GL_FINISH") != NULL;
    if (s_finish) {
        glFinish();
        if (s_new_obj)
            fprintf(stderr, "  [GL] %s done\n", what);
        else if (what[0] == 's') {        /* "swap": one line a frame, a heartbeat */
            static unsigned swaps;
            fprintf(stderr, "  [GL] frame %u swapped\n", ++swaps);
        }
    }
    if (!upload)
        s_new_obj = 0;                    /* the draw that uses it reports too */
}
static GLuint        s_vao, s_vbo, s_ibo;
/* Streaming vertex / index buffers (see ring_alloc). */
typedef struct { GLuint buf; GLenum target; GLsizeiptr cap, off; } GlRing;
static GlRing s_vring = { 0, GL_ARRAY_BUFFER, 16 << 20, 0 };
static GlRing s_iring = { 0, GL_ELEMENT_ARRAY_BUFFER, 4 << 20, 0 };
static const uint32_t *s_regs;          /* the executor's method shadow */
static uint32_t      s_frame;
static GLuint        s_cur_prog;         /* what glUseProgram last got */
static void state_dirty(void);
/* Last sampler state set on a GL texture (bind_stage). */
typedef struct { GLuint id; uint32_t key; } TexParam;
#define TEX_PARAM_SLOTS 4096
static TexParam s_tex_param[TEX_PARAM_SLOTS];
static void tex_param_forget(GLuint id)
{
    TexParam *tp = &s_tex_param[id % TEX_PARAM_SLOTS];
    if (tp->id == id)
        tp->id = 0;
}

/* ── Surfaces ──────────────────────────────────────────────────────── */

typedef struct {
    uint32_t va, w, h;
    GLuint   fbo, tex, ds;
    uint32_t used;
    uint32_t aa_sx, aa_sy;
} GlSurf;

#define GL_MAX_SURF 32
static GlSurf  s_surf[GL_MAX_SURF];
static GlSurf *s_last;                  /* last surface drawn or cleared */
static int     s_drew_any;              /* the title has drawn something */

static GlSurf *surf_find(uint32_t va)
{
    int i;
    for (i = 0; i < GL_MAX_SURF; i++)
        if (s_surf[i].fbo && s_surf[i].va == va)
            return &s_surf[i];
    return NULL;
}

static GlSurf *surf_get(uint32_t va, uint32_t w, uint32_t h,
                        uint32_t aa_sx, uint32_t aa_sy)
{
    GlSurf *s = surf_find(va), *victim = NULL;
    int i;

    if (s && s->w == w && s->h == h) {
        s->used = s_frame;
        return s;
    }
    if (!s) {
        for (i = 0; i < GL_MAX_SURF; i++) {
            if (!s_surf[i].fbo) { victim = &s_surf[i]; break; }
            if (!victim || s_surf[i].used < victim->used) victim = &s_surf[i];
        }
        s = victim;
    }
    if (s->fbo) {
        glDeleteFramebuffers(1, &s->fbo);
        glDeleteTextures(1, &s->tex);
        glDeleteRenderbuffers(1, &s->ds);
    }
    memset(s, 0, sizeof *s);
    s_new_obj = 1;
    s->va = va; s->w = w; s->h = h; s->used = s_frame;
    s->aa_sx = aa_sx; s->aa_sy = aa_sy;
    glGenTextures(1, &s->tex);
    tex_param_forget(s->tex);
    glBindTexture(GL_TEXTURE_2D, s->tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0,
                 GL_BGRA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glGenRenderbuffers(1, &s->ds);
    glBindRenderbuffer(GL_RENDERBUFFER, s->ds);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, (GLsizei)w, (GLsizei)h);
    glGenFramebuffers(1, &s->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, s->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s->tex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                              GL_RENDERBUFFER, s->ds);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        fprintf(stderr, "  [GL] surface 0x%08X %ux%u incomplete\n", va, w, h);
    glViewport(0, 0, (GLsizei)w, (GLsizei)h);
    glClearColor(0, 0, 0, 1);
    glClearDepth(1.0);
    glClearStencil(0);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(1, 1, 1, 1);
    glDepthMask(1);
    glStencilMask(0xFF);
    state_dirty();
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    if (s_trace)
        fprintf(stderr, "  [GL] surface 0x%08X %ux%u (aa %ux%u)\n", va, w, h, aa_sx, aa_sy);
    return s;
}

static GlSurf *surf_bind(const Nv2aSurface *sf)
{
    uint32_t bpp = sf->bytes_per_pixel ? sf->bytes_per_pixel : 4;
    uint32_t w = sf->pitch ? sf->pitch / bpp : sf->width;
    uint32_t h = sf->height;
    GlSurf *s;

    if (w < sf->width) w = sf->width;
    if (!w || !h || !sf->color_va)
        return NULL;
    s = surf_get(sf->color_va, w, h, sf->aa_sx ? sf->aa_sx : 1,
                 sf->aa_sy ? sf->aa_sy : 1);
    glBindFramebuffer(GL_FRAMEBUFFER, s->fbo);
    glViewport(0, 0, (GLsizei)w, (GLsizei)h);
    s_last = s;
    return s;
}

/* ── Textures ─────────────────────────────────────────────────────── */

typedef struct {
    uint32_t va, color, w, h, pitch, hash, bytes;
    GLuint   tex;
    uint32_t used, checked;
} GlTex;

#define GL_MAX_TEX 1024
static GlTex     s_tex[GL_MAX_TEX];
static uint32_t *s_decode;
static size_t    s_decode_cap;

static int tex_size_from_format(uint32_t color)
{
    /* Swizzled (0x00-0x0B, 0x19, 0x1A, 0x27-0x3F except linear) and DXT
     * carry their size in the format word; linear images use IMAGE_RECT. */
    if (color >= 0x0C && color <= 0x0F)
        return 1;
    switch (color) {
    case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15:
    case 0x16: case 0x17: case 0x18: case 0x1B: case 0x1C: case 0x1D:
    case 0x1E: case 0x1F: case 0x20: case 0x24: case 0x25: case 0x26:
    case 0x2E: case 0x2F: case 0x30: case 0x31: case 0x34: case 0x35:
    case 0x36: case 0x37: case 0x40: case 0x41: case 0x43: case 0x44:
    case 0x45: case 0x46:
        return 0;
    default:
        return 1;
    }
}

static uint32_t bytes_hash(uint32_t va, uint32_t bytes)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset() + va;
    uint32_t h = 2166136261u ^ bytes, step, i;

    if (!bytes)
        return h;
    step = bytes / 256;
    if (step < 4) step = 4;
    step &= ~3u;
    for (i = 0; i + 4 <= bytes; i += step) {
        uint32_t w;
        memcpy(&w, mem + i, 4);
        h = (h ^ w) * 16777619u;
    }
    memcpy(&i, mem + bytes - 4, 4);
    return (h ^ i) * 16777619u;
}

static uint32_t tex_bytes(uint32_t color, uint32_t w, uint32_t h, uint32_t pitch)
{
    if (color == 0x0C) return ((w + 3) / 4) * ((h + 3) / 4) * 8;
    if (color == 0x0E || color == 0x0F) return ((w + 3) / 4) * ((h + 3) / 4) * 16;
    if (pitch) return pitch * h;
    return w * h * 4;          /* upper bound for swizzled formats */
}

/* Xbox swizzle: texel (u, v) of a power-of-two image sits at the index made
 * by interleaving the coordinate bits, u in the even positions and v in the
 * odd ones, until the smaller dimension runs out; the larger one's remaining
 * bits follow on top. */
static uint32_t swizzle_index(uint32_t u, uint32_t v, uint32_t w, uint32_t h)
{
    uint32_t out = 0, bit = 0, mw = w - 1, mh = h - 1, m;

    for (m = 1; mw >= m || mh >= m; m <<= 1) {
        if (mw >= m) { if (u & m) out |= 1u << bit; bit++; }
        if (mh >= m) { if (v & m) out |= 1u << bit; bit++; }
    }
    return out;
}

/* SZ_I8_A8R8G8B8: 8-bit indices, swizzled, into an A8R8G8B8 palette named by
 * SET_TEXTURE_PALETTE (offset in the upper bits, length code in [3:2]:
 * 256, 128, 64 or 32 entries). The executor's decoder has no palette state,
 * so this format is decoded here. */
static uint32_t s_palette_reg;

static int decode_indexed(uint32_t va, uint32_t w, uint32_t h, uint32_t *out)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t pal_va = nv2a_pb_resolve(s_palette_reg & ~0x3Fu);
    uint32_t entries = 256u >> ((s_palette_reg >> 2) & 3);
    const uint8_t *idx = mem + va;
    uint32_t x, y;

    if (!(s_palette_reg & ~0x3Fu))
        return 0;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            uint32_t i = idx[swizzle_index(x, y, w, h)];
            uint32_t c;
            if (i >= entries)
                i = 0;
            memcpy(&c, mem + pal_va + i * 4, 4);
            out[y * w + x] = c;
        }
    return 1;
}

/* GPU memory held by the texture cache, and its budget. The cache was
 * bounded by entries (GL_MAX_TEX) only; 1024 large textures is gigabytes,
 * and a console out of GPU memory does not fail an allocation -- it hangs.
 * Over budget, the least recently used textures go. RECOMP_GL_TEX_MB sets
 * it (default 256). */
static uint64_t s_tex_bytes;

uint64_t nv2a_gl_texture_bytes(void) { return s_tex_bytes; }
uint32_t nv2a_gl_frame_count(void) { return s_frame; }

static void tex_account(GlTex *t, uint32_t bytes)
{
    static uint64_t budget;
    if (!budget) {
        const char *e = getenv("RECOMP_GL_TEX_MB");
        budget = (uint64_t)(e && atoi(e) > 0 ? atoi(e) : 256) << 20;
    }
    s_tex_bytes += (uint64_t)bytes - t->bytes;
    t->bytes = bytes;
    while (s_tex_bytes > budget) {
        GlTex *lru = NULL;
        uint32_t i;
        for (i = 0; i < GL_MAX_TEX; i++) {
            GlTex *c = &s_tex[i];
            if (c->tex && c != t && c->used != s_frame && (!lru || c->used < lru->used))
                lru = c;
        }
        if (!lru)
            break;                      /* everything left is in use this frame */
        glDeleteTextures(1, &lru->tex);
        s_tex_bytes -= lru->bytes;
        memset(lru, 0, sizeof *lru);
    }
}

static GLuint tex_get(uint32_t va, uint32_t color, uint32_t w, uint32_t h,
                      uint32_t pitch)
{
    GlTex *t = NULL, *victim = NULL;
    uint32_t i, hash;
    Nv2aTexture nt;

    for (i = 0; i < GL_MAX_TEX; i++) {
        GlTex *c = &s_tex[i];
        if (c->tex && c->va == va && c->color == color && c->w == w && c->h == h) {
            t = c;
            break;
        }
        if (!c->tex) { if (!victim || victim->tex) victim = c; }
        else if (!victim || (victim->tex && c->used < victim->used)) victim = c;
    }
    /* Once per frame per texture is enough to notice an update. */
    if (t && t->checked == s_frame) {
        t->used = s_frame;
        return t->tex;
    }
    hash = bytes_hash(va, tex_bytes(color, w, h, pitch));
    if (color == 0x0B)                       /* the palette is content too */
        hash ^= bytes_hash(nv2a_pb_resolve(s_palette_reg & ~0x3Fu), 1024) * 31u;
    if (t && t->hash == hash) {
        t->used = t->checked = s_frame;
        return t->tex;
    }
    if (!t) {
        t = victim;
        if (t->tex) {
            glDeleteTextures(1, &t->tex);
            s_tex_bytes -= t->bytes;
        }
        memset(t, 0, sizeof *t);
        glGenTextures(1, &t->tex);
        tex_param_forget(t->tex);
    } else if (color == 0x12 && t->pitch == pitch && pitch && !(pitch & 3)
               && pitch / 4 >= w) {
        /* A linear A8R8G8B8 image whose contents changed -- every frame of a
         * movie. That is GL's BGRA/UNSIGNED_BYTE byte for byte: update the
         * existing storage straight from guest memory, no decode, no
         * reallocation. */
        t->hash = hash; t->used = t->checked = s_frame;
        glBindTexture(GL_TEXTURE_2D, t->tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)(pitch / 4));
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)w, (GLsizei)h, GL_BGRA,
                        GL_UNSIGNED_BYTE, (const uint8_t *)xbox_GetMemoryOffset() + va);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        gl_step_done("texture upload");
        return t->tex;
    }
    t->va = va; t->color = color; t->w = w; t->h = h; t->pitch = pitch;
    t->hash = hash; t->used = t->checked = s_frame;
    s_new_obj = 1;
    if (s_trace)
        fprintf(stderr, "  [GL] texture 0x%08X format 0x%02X %ux%u pitch %u\n",
                va, color, w, h, pitch);

    if (color == 0x12 && pitch && !(pitch & 3) && pitch / 4 >= w) {
        glBindTexture(GL_TEXTURE_2D, t->tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)(pitch / 4));
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0, GL_BGRA,
                     GL_UNSIGNED_BYTE, (const uint8_t *)xbox_GetMemoryOffset() + va);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        tex_account(t, w * h * 4);
        return t->tex;
    }

    if ((size_t)w * h > s_decode_cap) {
        free(s_decode);
        s_decode_cap = (size_t)w * h;
        s_decode = (uint32_t *)malloc(s_decode_cap * 4);
    }
    memset(&nt, 0, sizeof nt);
    nt.offset = va; nt.width = w; nt.height = h; nt.pitch = pitch; nt.color = color;
    nt.addr_u = nt.addr_v = 1;
    glBindTexture(GL_TEXTURE_2D, t->tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (s_decode && (color == 0x0B ? decode_indexed(va, w, h, s_decode)
                                    : nv2a_backend_decode_texture(&nt, s_decode))) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0,
                     GL_BGRA, GL_UNSIGNED_BYTE, s_decode);
        tex_account(t, w * h * 4);
        gl_step_done("texture upload");
    } else {
        static const uint32_t magenta = 0xFFFF00FFu;
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_BGRA,
                     GL_UNSIGNED_BYTE, &magenta);
        tex_account(t, 4);
        if (s_trace)
            fprintf(stderr, "  [GL] texture 0x%08X format 0x%02X not decodable\n",
                    va, color);
    }
    return t->tex;
}

static GLint wrap_mode(uint32_t m)
{
    switch (m) {
    case 2:  return GL_MIRRORED_REPEAT;
    case 3:  return GL_CLAMP_TO_EDGE;
    case 4:  return GL_CLAMP_TO_BORDER;
    case 5:  return GL_CLAMP_TO_EDGE;
    default: return GL_REPEAT;
    }
}

/* Bind stage `i` from the method shadow; returns the texcoord scale. */
static void bind_stage(const uint32_t *regs, int i, float scale[2])
{
    uint32_t base = (0x1B00u + (uint32_t)i * 0x40u) / 4;
    uint32_t control0 = regs[base + 3];
    uint32_t format = regs[base + 1];
    uint32_t color = (format >> 8) & 0xFF;
    uint32_t va, w, h, pitch = 0;
    GLuint tex = 0;
    GlSurf *rt;

    scale[0] = scale[1] = 1.0f;
    glActiveTexture(GL_TEXTURE0 + (GLenum)i);
    if (!(control0 & 0x40000000u) || !regs[base]) {
        glBindTexture(GL_TEXTURE_2D, 0);
        return;
    }
    va = nv2a_pb_resolve(regs[base]);
    if (tex_size_from_format(color)) {
        w = 1u << ((format >> 20) & 0xF);
        h = 1u << ((format >> 24) & 0xF);
    } else {
        uint32_t rect = regs[base + 7];
        w = rect >> 16;
        h = rect & 0xFFFF;
        pitch = regs[base + 4] >> 16;
        if (w) scale[0] = 1.0f / (float)w;
        if (h) scale[1] = 1.0f / (float)h;
    }
    if (!w || !h || w > 4096 || h > 4096) {
        glBindTexture(GL_TEXTURE_2D, 0);
        return;
    }
    s_palette_reg = regs[base + 8];          /* SET_TEXTURE_PALETTE */
    rt = surf_find(va);
    if (rt) {
        tex = rt->tex;
        /* The image the title samples is the logical one; the FBO holds it
         * at the anti-aliased size. Normalised coordinates cover both. */
    } else {
        tex = tex_get(va, color, w, h, pitch);
    }
    glBindTexture(GL_TEXTURE_2D, tex);
    {
        /* Sampler state lives in the texture object: set it only when it
         * differs from what that texture last got. A texture id reused for a
         * new texture starts from GL's defaults, so the cache entry is keyed
         * by id and invalidated where textures are created (tex_param_forget). */
        uint32_t addr = regs[base + 2];
        uint32_t filter = regs[base + 5];
        GLint mag = ((filter >> 24) & 0xF) == 1 ? GL_NEAREST : GL_LINEAR;
        GLint min = ((filter >> 16) & 0xFF) == 1 ? GL_NEAREST : GL_LINEAR;
        uint32_t key = (addr & 0xF0F) | ((uint32_t)(mag == GL_NEAREST) << 16)
                     | ((uint32_t)(min == GL_NEAREST) << 17) | 0x80000000u;
        TexParam *tp = &s_tex_param[tex % TEX_PARAM_SLOTS];
        if (tp->id != tex || tp->key != key) {
            tp->id = tex;
            tp->key = key;
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap_mode(addr & 0xF));
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap_mode((addr >> 8) & 0xF));
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mag);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min);
        }
    }
}

/* ── Programs ─────────────────────────────────────────────────────── */

typedef struct {
    uint64_t   vkey;            /* 0: fixed-function; else program hash */
    Nv2aPshKey pkey;
    GLuint     prog;
    GLint      u_c, u_surf, u_aa, u_m, u_vpoff, u_xform;
    GLint      u_t[4], u_tscale, u_c0, u_c1, u_fc0, u_fc1, u_fogcolor, u_afunc, u_aref;
    uint32_t   used;
    int        vpc_valid;       /* vpc holds what this program's "c" was given */
    float      vpc[192][4];
    int        uni_valid;       /* uni holds the rest of its uniforms */
    struct GlUni {
        float surf[4], aa[2], m[16], vpoff[4], tscale[4][2];
        float c0[8][4], c1[8][4], fog[4], fc[2][4], aref;
        int   xform, afunc;
    } uni;
} GlProg;

#define GL_MAX_PROG 1024
static GlProg s_prog[GL_MAX_PROG];
static uint32_t s_prog_made, s_prog_live;

uint32_t nv2a_gl_program_count(void) { return s_prog_live; }

static GLuint compile(GLenum type, const char *const *src, int n)
{
    GLuint sh = glCreateShader(type);
    GLint ok = 0;

    glShaderSource(sh, n, src, NULL);
    glCompileShader(sh);
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        int i;
        glGetShaderInfoLog(sh, sizeof log, NULL, log);
        fprintf(stderr, "  [GL] %s shader failed:\n%s\n",
                type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
        if (s_trace)
            for (i = 0; i < n; i++)
                fprintf(stderr, "%s", src[i]);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static uint64_t prog_hash(const Nv2aRawBatch *b)
{
    uint64_t h = 1469598103934665603ull;
    uint32_t pc, k;

    for (pc = b->vp_start; pc < b->vp_slots; pc++) {
        for (k = 0; k < 4; k++)
            h = (h ^ b->vp_program[pc][k]) * 1099511628211ull;
        if (b->vp_program[pc][3] & 1)           /* FINAL */
            break;
    }
    return h ? h : 1;
}

static GlProg *prog_get(const Nv2aRawBatch *b, const Nv2aPshKey *pk)
{
    uint64_t vkey = b->xform == 2 ? prog_hash(b) : 0;
    GlProg *p = NULL, *victim = NULL;
    static char vbody[256 * 1024], fsrc[64 * 1024];
    const char *vs[4];
    int nvs = 0, i;
    GLuint v, fr;
    GLint ok = 0;

    for (i = 0; i < GL_MAX_PROG; i++) {
        GlProg *c = &s_prog[i];
        if (c->prog && c->vkey == vkey && !memcmp(&c->pkey, pk, sizeof *pk)) {
            c->used = s_frame;
            return c;
        }
        if (!c->prog) { if (!victim || victim->prog) victim = c; }
        else if (!victim || (victim->prog && c->used < victim->used)) victim = c;
    }
    p = victim;
    if (p->prog) {
        /* Delete it, not just forget it: an evicted program left alive
         * keeps its code in the driver's GPU code heap for good, and on the
         * Switch's Mesa a full code heap is a hung GPU. */
        glUseProgram(0);
        s_cur_prog = 0;
        glDeleteProgram(p->prog);
        s_prog_live--;
    }
    memset(p, 0, sizeof *p);
    if ((++s_prog_made % 100) == 0)
        fprintf(stderr, "  [GL] %u shader programs compiled (%u live)\n",
                s_prog_made, s_prog_live + 1);

    vs[nvs++] = nv2a_gl_vsh_prelude();
    if (vkey) {
        if (nv2a_gl_vsh_program(b->vp_program, b->vp_slots, b->vp_start,
                                vbody, sizeof vbody) < 0)
            return NULL;
        vs[nvs++] = vbody;
        vs[nvs++] = nv2a_gl_vsh_main_program();
    } else {
        vs[nvs++] = nv2a_gl_vsh_fixed();
    }
    if (nv2a_gl_psh(pk, fsrc, sizeof fsrc) < 0)
        return NULL;
    v = compile(GL_VERTEX_SHADER, vs, nvs);
    {
        const char *fs[1] = { fsrc };
        fr = compile(GL_FRAGMENT_SHADER, fs, 1);
    }
    if (!v || !fr)
        return NULL;
    p->prog = glCreateProgram();
    glAttachShader(p->prog, v);
    glAttachShader(p->prog, fr);
    glLinkProgram(p->prog);
    glDeleteShader(v);
    glDeleteShader(fr);
    glGetProgramiv(p->prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(p->prog, sizeof log, NULL, log);
        fprintf(stderr, "  [GL] link failed: %s\n", log);
        glDeleteProgram(p->prog);
        p->prog = 0;
        return NULL;
    }
    s_prog_live++;
    s_new_obj = 1;
    p->vkey = vkey;
    p->pkey = *pk;
    p->used = s_frame;
    p->u_c = glGetUniformLocation(p->prog, "c");
    p->u_surf = glGetUniformLocation(p->prog, "u_surf");
    p->u_aa = glGetUniformLocation(p->prog, "u_aa");
    p->u_m = glGetUniformLocation(p->prog, "u_m");
    p->u_vpoff = glGetUniformLocation(p->prog, "u_vpoff");
    p->u_xform = glGetUniformLocation(p->prog, "u_xform");
    p->u_tscale = glGetUniformLocation(p->prog, "u_tscale");
    p->u_c0 = glGetUniformLocation(p->prog, "u_c0");
    p->u_c1 = glGetUniformLocation(p->prog, "u_c1");
    p->u_fogcolor = glGetUniformLocation(p->prog, "u_fogcolor");
    p->u_fc0 = glGetUniformLocation(p->prog, "u_fc0");
    p->u_fc1 = glGetUniformLocation(p->prog, "u_fc1");
    p->u_afunc = glGetUniformLocation(p->prog, "u_alpha_func");
    p->u_aref = glGetUniformLocation(p->prog, "u_alpha_ref");
    glUseProgram(p->prog);
    s_cur_prog = p->prog;
    for (i = 0; i < 4; i++) {
        char n[4] = { 't', (char)('0' + i), 0, 0 };
        p->u_t[i] = glGetUniformLocation(p->prog, n);
        if (p->u_t[i] >= 0)
            glUniform1i(p->u_t[i], i);
    }
    if (s_trace) {
        static int shown;
        if (shown++ < 4)
            fprintf(stderr, "  [GL] program %016llX linked\n", (unsigned long long)vkey);
    }
    return p;
}

/* ── Context ──────────────────────────────────────────────────────── */

static int ready(void)
{
    if (s_state)
        return s_state > 0;
    s_state = -1;
    s_trace = getenv("RECOMP_GL_TRACE") != NULL;
    if (nv2a_gl_adopt_window((void **)&s_win, (void **)&s_ctx)) {
        /* The host already made the window and context (a loading screen)
         * and has let go of the context: take it on this thread. */
        if (SDL_GL_MakeCurrent(s_win, s_ctx) != 0) {
            fprintf(stderr, "  [GL] adopting the window: %s\n", SDL_GetError());
            return 0;
        }
    } else {
        if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
            fprintf(stderr, "  [GL] SDL video: %s\n", SDL_GetError());
            return 0;
        }
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        s_win = SDL_CreateWindow("NV2A", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                 1280, 720, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
        if (!s_win) {
            fprintf(stderr, "  [GL] window: %s\n", SDL_GetError());
            return 0;
        }
        s_ctx = SDL_GL_CreateContext(s_win);
        if (!s_ctx) {
            fprintf(stderr, "  [GL] context: %s\n", SDL_GetError());
            return 0;
        }
    }
    SDL_GL_SetSwapInterval(0);
    if (nv2a_gl_load(SDL_GL_GetProcAddress) != 0)
        return 0;
    fprintf(stderr, "  [GL] %s / %s / %s\n", (const char *)glGetString(GL_VENDOR),
            (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION));
    {
        /* Which size presenting will use. SDL reports the window as created;
         * a host with a better answer (the Switch's EGL surface, which
         * follows the display) supplies it through nv2a_gl_screen_size. */
        int dw = 0, dh = 0, sw = 0, sh = 0;
        SDL_GL_GetDrawableSize(s_win, &dw, &dh);
        if (!nv2a_gl_screen_size(&sw, &sh)) { sw = dw; sh = dh; }
        fprintf(stderr, "  [GL] screen %dx%d, SDL drawable %dx%d\n", sw, sh, dw, dh);
    }
    glGenVertexArrays(1, &s_vao);
    glBindVertexArray(s_vao);
    glGenBuffers(1, &s_vbo);
    glGenBuffers(1, &s_ibo);
    s_vring.buf = s_vbo;
    s_iring.buf = s_ibo;
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, s_vring.cap, NULL, GL_STREAM_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, s_iring.cap, NULL, GL_STREAM_DRAW);
    s_state = 1;
    return 1;
}

/* Called once, when the renderer first needs the display. A host that shows
 * something of its own while the title boots (the Switch's loading screen)
 * overrides this: it stops drawing, releases its GL context, and hands back
 * its SDL window and context (as void *) for the renderer to use. Returning
 * 0 lets the renderer create its own. */
__attribute__((weak)) int nv2a_gl_adopt_window(void **win, void **ctx)
{
    (void)win; (void)ctx;
    return 0;
}

/* Called on a present with nothing of the title's to show. Returns 1 if the
 * host drew something (its loading screen) into the default framebuffer. */
__attribute__((weak)) int nv2a_gl_draw_placeholder(void)
{
    return 0;
}

/* The on-screen size, if the host knows it better than SDL. */
__attribute__((weak)) int nv2a_gl_screen_size(int *w, int *h)
{
    (void)w; (void)h;
    return 0;
}

/* ── Render state ─────────────────────────────────────────────────── */

static float byte_f(uint32_t v, int shift) { return (float)((v >> shift) & 0xFF) / 255.0f; }

static void argb_vec4(uint32_t v, float out[4])
{
    out[0] = byte_f(v, 16); out[1] = byte_f(v, 8);
    out[2] = byte_f(v, 0);  out[3] = byte_f(v, 24);
}

static GLenum blend_eq(uint32_t v)
{
    switch (v) {
    case 0x800A: case 0x800B: case 0x8007: case 0x8008: return v;
    default: return GL_FUNC_ADD;             /* also the signed variants */
    }
}

/* The render state as apply_state sets it. Consecutive draws mostly share
 * it, and re-setting ~20 pieces of GL state per draw -- about 900 draws a
 * frame in a race -- is driver work for nothing. Anything else that touches
 * this state (clears, presents, new surfaces) calls state_dirty(). */
typedef struct {
    uint32_t blend, bsrc, bdst, beq, bcolor;
    uint32_t depth, dfunc, dmask;
    uint32_t stencil, smask, sfunc, sref, sread, sop[3];
    uint32_t cull, cullface, front, cm;
} GlState;
static GlState s_st;
static int s_st_valid;

static void state_dirty(void) { s_st_valid = 0; }

static void apply_state(const uint32_t *r, int has_depth)
{
    uint32_t cm = r[0x358 / 4];
    GlState want;

    memset(&want, 0, sizeof want);
    if (r[0x304 / 4]) {
        want.blend = 1; want.bsrc = r[0x344 / 4]; want.bdst = r[0x348 / 4];
        want.beq = r[0x350 / 4]; want.bcolor = r[0x34C / 4];
    }
    if (has_depth && r[0x30C / 4]) {
        want.depth = 1; want.dfunc = r[0x354 / 4];
    }
    want.dmask = has_depth && r[0x35C / 4];
    if (has_depth && r[0x32C / 4]) {
        want.stencil = 1; want.smask = r[0x360 / 4]; want.sfunc = r[0x364 / 4];
        want.sref = r[0x368 / 4]; want.sread = r[0x36C / 4];
        want.sop[0] = r[0x370 / 4]; want.sop[1] = r[0x374 / 4]; want.sop[2] = r[0x378 / 4];
    }
    if (r[0x308 / 4]) {
        want.cull = 1; want.cullface = r[0x39C / 4]; want.front = r[0x3A0 / 4];
    }
    want.cm = cm;
    if (s_st_valid && !memcmp(&want, &s_st, sizeof want))
        return;
    s_st = want;
    s_st_valid = 1;

    if (r[0x304 / 4]) {
        float bc[4];
        glEnable(GL_BLEND);
        glBlendFunc(r[0x344 / 4], r[0x348 / 4]);
        glBlendEquation(blend_eq(r[0x350 / 4]));
        argb_vec4(r[0x34C / 4], bc);
        glBlendColor(bc[0], bc[1], bc[2], bc[3]);
    } else {
        glDisable(GL_BLEND);
    }
    if (has_depth && r[0x30C / 4]) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(r[0x354 / 4] ? r[0x354 / 4] : 0x203);
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    glDepthMask(has_depth && r[0x35C / 4] ? 1 : 0);
    if (has_depth && r[0x32C / 4]) {
        glEnable(GL_STENCIL_TEST);
        glStencilMask(r[0x360 / 4] & 0xFF);
        glStencilFunc(r[0x364 / 4] ? r[0x364 / 4] : GL_ALWAYS, (GLint)(r[0x368 / 4] & 0xFF),
                      r[0x36C / 4] & 0xFF);
        glStencilOp(r[0x370 / 4] ? r[0x370 / 4] : GL_KEEP,
                    r[0x374 / 4] ? r[0x374 / 4] : GL_KEEP,
                    r[0x378 / 4] ? r[0x378 / 4] : GL_KEEP);
    } else {
        glDisable(GL_STENCIL_TEST);
    }
    if (r[0x308 / 4]) {
        glEnable(GL_CULL_FACE);
        glCullFace(r[0x39C / 4] ? r[0x39C / 4] : GL_BACK);
        /* Rows are top-first here, which mirrors the winding. */
        glFrontFace(r[0x3A0 / 4] == GL_CCW ? GL_CW : GL_CCW);
    } else {
        glDisable(GL_CULL_FACE);
    }
    glColorMask((cm & 0x00010000u) != 0, (cm & 0x00000100u) != 0,
                (cm & 0x00000001u) != 0, (cm & 0x01000000u) != 0);
    glDisable(GL_SCISSOR_TEST);
    /* The NV2A does not clip against the near and far planes; it clamps
     * depth. GL clips there, so a screen quad placed at the far plane
     * (NFSU2's loading screen, z = 1.0 through its vertex program) vanished
     * whenever rounding put it a hair beyond. */
    glEnable(0x864F);                               /* GL_DEPTH_CLAMP */
}

static void psh_key(const uint32_t *r, Nv2aPshKey *k)
{
    int i;

    memset(k, 0, sizeof *k);
    for (i = 0; i < 8; i++) {
        k->color_icw[i] = r[0xAC0 / 4 + i];
        k->color_ocw[i] = r[0x1E40 / 4 + i];
        k->alpha_icw[i] = r[0x260 / 4 + i];
        k->alpha_ocw[i] = r[0xAA0 / 4 + i];
    }
    k->control = r[0x1E60 / 4];
    k->final0 = r[0x288 / 4];
    k->final1 = r[0x28C / 4];
    k->shader_program = r[0x1E70 / 4];
    k->other_input = r[0x1E78 / 4];
}

/* NV097 primitive -> GL, rewriting the ones core GL lacks. */
static uint32_t *s_prim_idx;
static size_t    s_prim_cap;

static GLenum gl_prim(const Nv2aRawBatch *b, const uint32_t **idx, uint32_t *n)
{
    uint32_t i, m = 0;

    *idx = b->indices;
    *n = b->index_count;
    switch (b->prim) {
    case 1: return 0x0000;             /* POINTS */
    case 2: return 0x0001;             /* LINES */
    case 3: return 0x0002;             /* LINE_LOOP */
    case 4: return 0x0003;             /* LINE_STRIP */
    case 5: return GL_TRIANGLES;
    case 6: return 0x0005;             /* TRIANGLE_STRIP */
    case 7: case 10: return 0x0006;    /* TRIANGLE_FAN, POLYGON */
    case 9: return 0x0005;             /* QUAD_STRIP == triangle strip */
    case 8: {                          /* QUADS -> triangles */
        size_t need = (size_t)b->index_count / 4 * 6;
        if (need > s_prim_cap) {
            free(s_prim_idx);
            s_prim_cap = need + 1024;
            s_prim_idx = (uint32_t *)malloc(s_prim_cap * 4);
        }
        if (!s_prim_idx) { *n = 0; return GL_TRIANGLES; }
        for (i = 0; i + 3 < b->index_count; i += 4) {
            const uint32_t *q = b->indices + i;
            s_prim_idx[m++] = q[0]; s_prim_idx[m++] = q[1]; s_prim_idx[m++] = q[2];
            s_prim_idx[m++] = q[0]; s_prim_idx[m++] = q[2]; s_prim_idx[m++] = q[3];
        }
        *idx = s_prim_idx;
        *n = m;
        return GL_TRIANGLES;
    }
    default:
        *n = 0;
        return GL_TRIANGLES;
    }
}

/* ── Back end entry points ────────────────────────────────────────── */

static uint32_t zmax_of(const uint32_t *r)
{
    return (((r[0x208 / 4] >> 4) & 0xF) == 1) ? 0xFFFFu : 0xFFFFFFu;
}

/* Streaming vertex and index data.
 *
 * glBufferData per draw re-specifies the buffer every time -- a driver
 * allocation, twice per draw, ~900 draws a frame. Instead each buffer is one
 * large allocation filled front to back: a draw maps its own range without
 * synchronisation (nothing the GPU may still read is ever overwritten) and
 * the buffer is orphaned only when it is full. */

/* Space for `size` bytes; returns the offset, and a pointer to write them to
 * (NULL if mapping failed: then ring_put the data instead). */
static GLintptr ring_alloc(GlRing *g, GLsizeiptr size, void **ptr)
{
    GLintptr at;

    glBindBuffer(g->target, g->buf);
    if (size > g->cap) {
        g->cap = size * 2;
        g->off = g->cap;                    /* forces the orphan below */
    }
    at = (g->off + 63) & ~(GLintptr)63;
    if (at + size > g->cap) {
        glBufferData(g->target, g->cap, NULL, GL_STREAM_DRAW);
        at = 0;
    }
    g->off = at + size;
    *ptr = glMapBufferRange(g->target, at, size,
                            GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT
                            | GL_MAP_INVALIDATE_RANGE_BIT);
    return at;
}

static void ring_done(GlRing *g, GLintptr at, GLsizeiptr size, void *ptr,
                      const void *fallback)
{
    if (ptr)
        glUnmapBuffer(g->target);
    else if (fallback)
        glBufferSubData(g->target, at, size, fallback);
}

static void gl_draw_raw(const Nv2aRawBatch *b)
{
    GlSurf *s;
    GlProg *p;
    Nv2aPshKey pk;
    const uint32_t *r = b->regs;
    const uint32_t *idx;
    uint32_t n, a, i;
    GLintptr idx_at;
    GLenum mode;
    float tscale[4][2], c0[8][4], c1[8][4], fog[4], surf[4], aa[2], m[16];

    if (!ready())
        return;
    s_regs = r;
    s = surf_bind(&b->surface);
    if (!s)
        return;
    s_drew_any = 1;
    psh_key(r, &pk);
    p = prog_get(b, &pk);
    if (!p)
        return;
    if (p->prog != s_cur_prog) {
        glUseProgram(p->prog);
        s_cur_prog = p->prog;
    }

    for (i = 0; i < 4; i++)
        bind_stage(r, (int)i, tscale[i]);
    for (i = 0; i < 8; i++) {
        argb_vec4(r[0xA60 / 4 + i], c0[i]);
        argb_vec4(r[0xA80 / 4 + i], c1[i]);
    }
    {
        uint32_t fc = r[0x2A8 / 4];            /* R in the low byte */
        fog[0] = byte_f(fc, 0); fog[1] = byte_f(fc, 8);
        fog[2] = byte_f(fc, 16); fog[3] = byte_f(fc, 24);
    }
    surf[0] = 2.0f / (float)s->w;
    surf[1] = 2.0f / (float)s->h;
    surf[2] = 1.0f / (float)zmax_of(r);
    surf[3] = 0.0f;
    aa[0] = b->aa_sx > 0 ? b->aa_sx : 1.0f;
    aa[1] = b->aa_sy > 0 ? b->aa_sy : 1.0f;
    memcpy(m, b->composite, sizeof m);

    /* 192 constants are 3 KB, and most draws in a row share them: upload
     * only when they changed since this program last got them. */
    if (p->u_c >= 0 && (!p->vpc_valid
                        || memcmp(p->vpc, b->vp_consts, sizeof p->vpc) != 0)) {
        memcpy(p->vpc, b->vp_consts, sizeof p->vpc);
        p->vpc_valid = 1;
        glUniform4fv(p->u_c, 192, &b->vp_consts[0][0]);
    }
    {
        /* The rest, likewise: set only when something differs from what this
         * program already holds. */
        struct GlUni u;
        memset(&u, 0, sizeof u);
        memcpy(u.surf, surf, sizeof u.surf);
        memcpy(u.aa, aa, sizeof u.aa);
        memcpy(u.m, m, sizeof u.m);
        memcpy(u.vpoff, b->vp_offset, sizeof u.vpoff);
        memcpy(u.tscale, tscale, sizeof u.tscale);
        memcpy(u.c0, c0, sizeof u.c0);
        memcpy(u.c1, c1, sizeof u.c1);
        memcpy(u.fog, fog, sizeof u.fog);
        argb_vec4(r[0x1E20 / 4], u.fc[0]);      /* SPECULAR_FOG_FACTOR0/1 */
        argb_vec4(r[0x1E24 / 4], u.fc[1]);
        u.aref = (float)(r[0x340 / 4] & 0xFF);
        u.xform = b->xform == 1 ? 1 : 0;
        u.afunc = r[0x300 / 4] ? (GLint)r[0x33C / 4] : 0x207;
        if (!p->uni_valid || memcmp(&u, &p->uni, sizeof u) != 0) {
            p->uni = u;
            p->uni_valid = 1;
            if (p->u_surf >= 0) glUniform4fv(p->u_surf, 1, u.surf);
            if (p->u_aa >= 0) glUniform2fv(p->u_aa, 1, u.aa);
            if (p->u_m >= 0) glUniform4fv(p->u_m, 4, u.m);
            if (p->u_vpoff >= 0) glUniform4fv(p->u_vpoff, 1, u.vpoff);
            if (p->u_xform >= 0) glUniform1i(p->u_xform, u.xform);
            if (p->u_tscale >= 0) glUniform2fv(p->u_tscale, 4, &u.tscale[0][0]);
            if (p->u_c0 >= 0) glUniform4fv(p->u_c0, 8, &u.c0[0][0]);
            if (p->u_c1 >= 0) glUniform4fv(p->u_c1, 8, &u.c1[0][0]);
            if (p->u_fogcolor >= 0) glUniform4fv(p->u_fogcolor, 1, u.fog);
            if (p->u_fc0 >= 0) glUniform4fv(p->u_fc0, 1, u.fc[0]);
            if (p->u_fc1 >= 0) glUniform4fv(p->u_fc1, 1, u.fc[1]);
            if (p->u_afunc >= 0) glUniform1i(p->u_afunc, u.afunc);
            if (p->u_aref >= 0) glUniform1f(p->u_aref, u.aref);
        }
    }

    apply_state(r, b->zeta_va != 0);

    glBindVertexArray(s_vao);
    {
        /* Upload only the attributes the batch has: the batch keeps all 16
         * per vertex (256 bytes), and a draw typically uses three or four,
         * so sending the lot was mostly copying zeros through the driver. */
        static float *pack;
        static size_t pack_cap;
        uint32_t np = 0, v, slot[NV2A_RAW_ATTRS];
        GLsizeiptr bytes;
        GLintptr at;
        void *dst;
        float *out;

        for (a = 0; a < NV2A_RAW_ATTRS; a++)
            if (b->attr_present & (1u << a))
                slot[np++] = a;
        bytes = (GLsizeiptr)b->vertex_count * (np ? np : 1) * 4 * sizeof(float);
        if (!bytes)
            return;
        at = ring_alloc(&s_vring, bytes, &dst);
        out = (float *)dst;
        if (!out) {
            if ((size_t)bytes > pack_cap) {
                free(pack);
                pack_cap = (size_t)bytes + 65536;
                pack = (float *)malloc(pack_cap);
                if (!pack) { pack_cap = 0; return; }
            }
            out = pack;
        }
        for (v = 0; v < b->vertex_count; v++)
            for (i = 0; i < np; i++)
                memcpy(out + ((size_t)v * np + i) * 4,
                       b->attrs + ((size_t)v * NV2A_RAW_ATTRS + slot[i]) * 4,
                       4 * sizeof(float));
        ring_done(&s_vring, at, bytes, dst, pack);
        for (i = 0; i < np; i++) {
            glEnableVertexAttribArray(slot[i]);
            glVertexAttribPointer(slot[i], 4, GL_FLOAT, GL_FALSE,
                                  (GLsizei)(np * 4 * sizeof(float)),
                                  (const void *)(uintptr_t)(at + i * 4 * sizeof(float)));
        }
    }
    {
        /* The absent attributes' constant values change only with the set
         * of attributes (and diffuse with the transform mode). */
        static uint32_t last_mask = 0xFFFFFFFFu, last_ffp = 2;
        uint32_t ffp = b->xform != 2;
        if (b->attr_present != last_mask || ffp != last_ffp) {
            last_mask = b->attr_present;
            last_ffp = ffp;
            for (a = 0; a < NV2A_RAW_ATTRS; a++) {
                if (b->attr_present & (1u << a))
                    continue;
                glDisableVertexAttribArray(a);
                if (a == 3 && ffp)
                    glVertexAttrib4f(a, 1.0f, 1.0f, 1.0f, 1.0f);
                else
                    glVertexAttrib4f(a, 0.0f, 0.0f, 0.0f, 1.0f);
            }
        }
    }
    mode = gl_prim(b, &idx, &n);
    if (!n)
        return;
    {
        void *dst;
        idx_at = ring_alloc(&s_iring, (GLsizeiptr)n * 4, &dst);
        if (dst)
            memcpy(dst, idx, (size_t)n * 4);
        ring_done(&s_iring, idx_at, (GLsizeiptr)n * 4, dst, idx);
    }
    {
        /* RECOMP_GL_FINISH=1: name each draw before it is submitted and wait
         * for the GPU to finish it. With a synchronous log, the last line
         * of a hard GPU hang is the draw that caused it. Slow. */
        static uint32_t draws;
        if (s_finish < 0)
            s_finish = getenv("RECOMP_GL_FINISH") != NULL;
        ++draws;
        if (s_finish && s_new_obj)
            fprintf(stderr, "  [GL] draw %u: surf %08X %ux%u prim %u idx %u verts %u attrs %04X"
                    " xform %u vp %u shaders %08X combiners %08X tex0 %08X/%02X\n",
                    draws, s->va, s->w, s->h, b->prim, n, b->vertex_count,
                    b->attr_present, b->xform, b->vp_start, pk.shader_program, pk.control,
                    r[0x1B00 / 4], (r[0x1B04 / 4] >> 8) & 0xFF);
        glDrawElements(mode, (GLsizei)n, GL_UNSIGNED_INT, (const void *)(uintptr_t)idx_at);
        gl_step_done("draw");
    }
}

static void gl_clear(const Nv2aSurface *sf, const Nv2aRenderState *rs,
                     uint32_t flags, uint32_t argb, uint32_t zstencil)
{
    GlSurf *s;
    GLbitfield bits = 0;
    float c[4];

    state_dirty();

    (void)rs;
    if (!ready())
        return;
    s = surf_bind(sf);
    if (!s)
        return;
    glDisable(GL_SCISSOR_TEST);
    if (s_regs) {
        uint32_t hz = s_regs[0x1D98 / 4], vt = s_regs[0x1D9C / 4];
        /* The clear rectangle is in logical pixels, like SURFACE_CLIP; the
         * surface is larger by the anti-aliasing factor. Unscaled, a 2x1
         * surface cleared only its left half, and stale depth on the right
         * kept old frames of a moving car alive behind the background. */
        uint32_t x0 = (hz & 0xFFFF) * s->aa_sx, x1 = ((hz >> 16) + 1) * s->aa_sx;
        uint32_t y0 = (vt & 0xFFFF) * s->aa_sy, y1 = ((vt >> 16) + 1) * s->aa_sy;
        if (x1 > s->w) x1 = s->w;
        if (y1 > s->h) y1 = s->h;
        if (x1 > x0 && y1 > y0 && (x1 - x0 < s->w || y1 - y0 < s->h)) {
            glEnable(GL_SCISSOR_TEST);
            glScissor((GLint)x0, (GLint)y0, (GLsizei)(x1 - x0), (GLsizei)(y1 - y0));
        }
    }
    if (flags & 0xF0) {
        argb_vec4(argb, c);
        glColorMask((flags & 0x10) != 0, (flags & 0x20) != 0,
                    (flags & 0x40) != 0, (flags & 0x80) != 0);
        glClearColor(c[0], c[1], c[2], c[3]);
        bits |= GL_COLOR_BUFFER_BIT;
    }
    if (flags & 0x1) {
        uint32_t zmax = s_regs ? zmax_of(s_regs) : 0xFFFFFFu;
        uint32_t z = zmax == 0xFFFFu ? (zstencil & 0xFFFF) : (zstencil >> 8);
        glDepthMask(1);
        glClearDepth((double)z / (double)zmax);
        bits |= GL_DEPTH_BUFFER_BIT;
    }
    if (flags & 0x2) {
        glStencilMask(0xFF);
        glClearStencil((GLint)(zstencil & 0xFF));
        bits |= GL_STENCIL_BUFFER_BIT;
    }
    if (bits)
        glClear(bits);
    gl_step_done("clear");
    glDisable(GL_SCISSOR_TEST);
}

static void dump_bmp(const char *path, const uint8_t *bgra, uint32_t w, uint32_t h)
{
    FILE *f = fopen(path, "wb");
    uint8_t hdr[54] = { 'B', 'M' };
    uint32_t row = w * 3, pad = (4 - (row & 3)) & 3, size = 54 + (row + pad) * h, y, x;

    if (!f)
        return;
    memcpy(hdr + 2, &size, 4);
    hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1; hdr[28] = 24;
    fwrite(hdr, 1, 54, f);
    /* BMP rows are bottom-first; the surface is top-first. */
    for (y = h; y-- > 0; ) {
        const uint8_t *p = bgra + (size_t)y * w * 4;
        for (x = 0; x < w; x++)
            fwrite(p + x * 4, 1, 3, f);
        fwrite("\0\0\0", 1, pad, f);
    }
    fclose(f);
}

static void gl_flip(void)
{
    state_dirty();
    static int dump_every = -1;
    static char dump_prefix[256];
    static uint8_t *pix;
    static size_t pix_cap;
    int ww, wh;
    GlSurf *s = s_last;

    if (!ready())
        return;
    s_frame++;
    if (dump_every < 0) {
        const char *d = getenv("RECOMP_GL_DUMP");
        dump_every = 0;
        if (d && *d) {
            const char *comma = strchr(d, ',');
            size_t n = comma ? (size_t)(comma - d) : strlen(d);
            if (n >= sizeof dump_prefix) n = sizeof dump_prefix - 1;
            memcpy(dump_prefix, d, n);
            dump_prefix[n] = 0;
            dump_every = comma ? atoi(comma + 1) : 60;
            if (dump_every <= 0) dump_every = 60;
        }
    }
    if (!s || !s_drew_any) {
        /* Nothing to show yet -- no surface, or only cleared ones (a title
         * clears to black long before its first frame): the host's
         * placeholder (a loading screen), or
         * at least a clear -- a buffer never drawn to reaches the screen as
         * whatever the driver left in it. */
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glDisable(GL_SCISSOR_TEST);
        glColorMask(1, 1, 1, 1);
        if (!nv2a_gl_draw_placeholder()) {
            glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT);
        }
        glDisable(GL_SCISSOR_TEST);
        goto swap;
    }

    if (dump_every && (s_frame % (uint32_t)dump_every) == 0) {
        size_t need = (size_t)s->w * s->h * 4;
        if (need > pix_cap) {
            free(pix);
            pix_cap = need;
            pix = (uint8_t *)malloc(pix_cap);
        }
        if (pix) {
            char path[320];
            glBindFramebuffer(GL_READ_FRAMEBUFFER, s->fbo);
            glPixelStorei(GL_PACK_ALIGNMENT, 4);
            glReadPixels(0, 0, (GLsizei)s->w, (GLsizei)s->h, GL_BGRA,
                         GL_UNSIGNED_BYTE, pix);
            snprintf(path, sizeof path, "%s%05u.bmp", dump_prefix, s_frame);
            dump_bmp(path, pix, s->w, s->h);
        }
    }

    /* Present: the logical aspect (surface / AA factor; 16:9 for a
     * widescreen title), letterboxed, with
     * the top-first rows turned the right way up. */
    /* The host may know the real on-screen size better than SDL (the
     * Switch's EGL surface follows the display, SDL keeps the window's
     * creation size). */
    if (!nv2a_gl_screen_size(&ww, &wh) || ww <= 0 || wh <= 0)
        SDL_GL_GetDrawableSize(s_win, &ww, &wh);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, s->fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(1, 1, 1, 1);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    {
        float lw = (float)s->w / (float)s->aa_sx, lh = (float)s->h / (float)s->aa_sy;
        /* A widescreen title renders 640x480 anamorphic: show it at 16:9. */
        if (xbox_video_widescreen() && lw < lh * 1.5f)
            lw = lh * 16.0f / 9.0f;
        float scale = (float)ww / lw < (float)wh / lh ? (float)ww / lw : (float)wh / lh;
        int dw = (int)(lw * scale), dh = (int)(lh * scale);
        int dx = (ww - dw) / 2, dy = (wh - dh) / 2;
        /* Scissored to the picture: Switch Mesa (nouveau) runs a scaled,
         * flipped blit over more than the destination rectangle, and the
         * pillarbox bars filled with the source's edge columns stretched
         * sideways. Blits obey the scissor test, so the bars stay cleared. */
        glEnable(GL_SCISSOR_TEST);
        glScissor(dx, dy, dw, dh);
        glBlitFramebuffer(0, 0, (GLint)s->w, (GLint)s->h,
                          dx, dy + dh, dx + dw, dy, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        glDisable(GL_SCISSOR_TEST);
    }
swap:
    gl_step_done("present");
    SDL_GL_SwapWindow(s_win);
    gl_step_done("swap");
    {
        SDL_Event e;
        while (SDL_PollEvent(&e)) { }
    }
}

static const Nv2aBackend s_backend_gl = {
    gl_clear,
    NULL,              /* draw: everything arrives through draw_raw */
    gl_flip,
    gl_draw_raw,
};

void nv2a_gl_install(void)
{
    nv2a_backend_register(&s_backend_gl);
    fprintf(stderr, "  [GL] NV2A OpenGL renderer registered\n");
}
