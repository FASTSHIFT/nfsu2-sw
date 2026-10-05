/*
 * kms_present.c -- frames to the panel by KMS page flip, not eglSwapBuffers.
 *
 * On the R36S (Mali blob, SDL KMSDRM) eglSwapBuffers blocks the GL thread in
 * poll(/dev/mali0) until the previous frame's GPU work is done -- the
 * window surface is double-buffered and its buffer count is the driver's
 * (reVC docs/08 7, 09). Measured here: ~36 ms of a 137 ms race frame, on the
 * thread that sets the frame rate (docs/02 13).
 *
 * Instead the GL thread never swaps: it blits the frame into one of a few
 * scanout buffers of its own (gbm bos, imported as FBOs through dma-buf),
 * puts an EGL fence behind the blit and hands the bo to a flip thread, which
 * waits for the fence, page-flips and waits for the vblank that shows it.
 * The GL thread only waits when every bo is still queued or on screen.
 *
 * Everything the toolchain image has no headers for (drm, gbm, EGL) is
 * declared here and resolved with dlopen, like SDL does: libEGL.so is the
 * blob's (the linked libEGL.so.1 would be glvnd without a Mali vendor).
 * Device facts from reVC's present chain (re3-sdl docs/09 7.4): drmModeAddFB2
 * hangs on the blob's gbm (legacy drmModeAddFB works), PAGE_FLIP_ASYNC is
 * EINVAL on the 4.4 rockchip driver, the process's SDL fd is the DRM master,
 * and FBO bindings are per context (here there is only one).
 *
 * RECOMP_KMS_PRESENT=1 turns it on (SDL's kmsdrm video driver only);
 * RECOMP_KMS_BOS=2..3 (default 3). Orientation: a bo is a texture-backed FBO
 * whose row 0 is the first row scanned out, and surfaces hold rows
 * top-first, so the blit is straight -- not the vertically flipped one the
 * window (rows bottom-first) needs; on the R36S that flip came out upside
 * down, and mirroring x on top of it left the picture mirrored. For another
 * panel, RECOMP_KMS_FLIPX / RECOMP_KMS_FLIPY = 1 flip an axis. Anything missing falls back to SDL_GL_SwapWindow.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1          /* RTLD_DEFAULT (glibc 2.31): before any header */
#endif
#include "kms_present.h"

#if defined(_WIN32) || defined(__SWITCH__)
int nv2a_kms_init(void *w) { (void)w; return 0; }
int nv2a_kms_active(void) { return 0; }
void nv2a_kms_mode(int *w, int *h) { *w = *h = 0; }
void nv2a_kms_present(GLuint s, int a, int b, int c, int d, int e, int f, int g, int h)
{ (void)s; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)h; }
#else
#include "platform/xtrace.h"

#include <SDL.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ── what the image has no headers for ─────────────────────────────── */

typedef struct {                       /* drmModeModeInfo */
    uint32_t clock;
    uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
    uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
    uint32_t vrefresh, flags, type;
    char name[32];
} KModeInfo;
typedef struct {                       /* drmModeRes: fbs, crtcs, connectors, encoders */
    int count_fbs; uint32_t *fbs;
    int count_crtcs; uint32_t *crtcs;
    int count_connectors; uint32_t *connectors;
    int count_encoders; uint32_t *encoders;
    uint32_t min_width, max_width, min_height, max_height;
} KRes;
typedef struct {                       /* drmModeConnector */
    uint32_t connector_id, encoder_id, connector_type, connector_type_id;
    int connection;                    /* 1 = connected */
    uint32_t mmWidth, mmHeight;
    int subpixel;
    int count_modes; KModeInfo *modes;
    int count_props; uint32_t *props; uint64_t *prop_values;
    int count_encoders; uint32_t *encoders;
} KConn;
typedef struct {                       /* drmModeEncoder */
    uint32_t encoder_id, encoder_type, crtc_id, possible_crtcs, possible_clones;
} KEnc;
typedef union {                        /* drmVBlank */
    struct { int type; unsigned int sequence; unsigned long signal; } request;
    struct { int type; unsigned int sequence; long tval_sec, tval_usec; } reply;
} KVBlank;
#define K_VBLANK_RELATIVE 0x1

typedef int EGLint;
typedef void *EGLDisplay, *EGLImage, *EGLSync;
#define K_EGL_NONE                   0x3038
#define K_EGL_WIDTH                  0x3057
#define K_EGL_HEIGHT                 0x3056
#define K_EGL_LINUX_DMA_BUF          0x3270
#define K_EGL_LINUX_DRM_FOURCC       0x3271
#define K_EGL_DMA_BUF_PLANE0_FD      0x3272
#define K_EGL_DMA_BUF_PLANE0_OFFSET  0x3273
#define K_EGL_DMA_BUF_PLANE0_PITCH   0x3274
#define K_EGL_SYNC_FENCE             0x30F9
#define K_EGL_SYNC_FLUSH_COMMANDS    0x0001
#define K_GBM_XRGB8888               0x34325258u      /* 'XR24' */
#define K_GBM_BO_USE_SCANOUT         (1u << 0)
#define K_GBM_BO_USE_RENDERING       (1u << 2)

static KRes  *(*k_GetResources)(int);
static void   (*k_FreeResources)(KRes *);
static KConn *(*k_GetConnector)(int, uint32_t);
static void   (*k_FreeConnector)(KConn *);
static KEnc  *(*k_GetEncoder)(int, uint32_t);
static void   (*k_FreeEncoder)(KEnc *);
static int    (*k_AddFB)(int, uint32_t, uint32_t, uint8_t, uint8_t, uint32_t, uint32_t, uint32_t *);
static int    (*k_PageFlip)(int, uint32_t, uint32_t, uint32_t, void *);
static int    (*k_WaitVBlank)(int, KVBlank *);
static void  *(*k_bo_create)(void *, uint32_t, uint32_t, uint32_t, uint32_t);
static uint32_t (*k_bo_stride)(void *);
static int    (*k_bo_fd)(void *);
static uint64_t (*k_bo_handle)(void *);            /* union gbm_bo_handle: 8 bytes in x0 */
static EGLDisplay (*k_eglGetCurrentDisplay)(void);
static void  *(*k_eglGetProcAddress)(const char *);
static EGLImage (*k_eglCreateImage)(EGLDisplay, void *, unsigned, void *, const EGLint *);
static EGLSync  (*k_eglCreateSync)(EGLDisplay, unsigned, const EGLint *);
static EGLint   (*k_eglClientWaitSync)(EGLDisplay, EGLSync, EGLint, uint64_t);
static unsigned (*k_eglDestroySync)(EGLDisplay, EGLSync);
static void (GLAPIENTRY *k_glEGLImageTargetTexture2DOES)(GLenum, void *);
static void (GLAPIENTRY *k_glFlush)(void);

/* ── state ─────────────────────────────────────────────────────────── */

#define KMS_MAX_BOS 3
static struct {
    int on, fd, w, h, flipx, flipy;
    uint32_t crtc;
    EGLDisplay dpy;
    int nbo;
    struct { void *bo; uint32_t fb; GLuint tex, fbo; } bo[KMS_MAX_BOS];
    /* bo states: 0 free, 1 queued for the flip thread, 2 on screen */
    int state[KMS_MAX_BOS];
    int queue[KMS_MAX_BOS]; EGLSync fence[KMS_MAX_BOS];
    int nq;
    pthread_mutex_t lock;
    pthread_cond_t cv;
    uint64_t flips, waits_ns, gl_waits_ns;
} K = { .lock = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER };

int nv2a_kms_active(void) { return K.on; }
void nv2a_kms_mode(int *w, int *h) { *w = K.w; *h = K.h; }

static void *sym(void *h, const char *name, int *missing)
{
    void *p = h ? dlsym(h, name) : NULL;
    if (!p) {
        fprintf(stderr, "  [KMS] missing %s\n", name);
        (*missing)++;
    }
    return p;
}

/* The flip thread: oldest queued bo -> wait for its blit -> flip -> wait for
 * the vblank that shows it; then every other bo is free. */
static void *flip_thread(void *arg)
{
    (void)arg;
    xtrace_thread_name("KMS flip");
    for (;;) {
        int b, k, tries;
        EGLSync f;
        pthread_mutex_lock(&K.lock);
        while (!K.nq)
            pthread_cond_wait(&K.cv, &K.lock);
        b = K.queue[0];
        f = K.fence[0];
        pthread_mutex_unlock(&K.lock);

        xtrace_begin("blit fence");
        if (f) {
            k_eglClientWaitSync(K.dpy, f, 0, 200000000ull);       /* 200 ms */
            k_eglDestroySync(K.dpy, f);
        }
        xtrace_end();
        xtrace_begin("page flip");
        for (tries = 0; k_PageFlip(K.fd, K.crtc, K.bo[b].fb, 0, NULL) != 0; tries++) {
            if (errno != EBUSY || tries > 50) {      /* 50 x 2 ms: give up on this one */
                fprintf(stderr, "  [KMS] page flip: %s\n", strerror(errno));
                break;
            }
            usleep(2000);
        }
        xtrace_end();
        xtrace_begin("vblank");
        {
            KVBlank vb;
            memset(&vb, 0, sizeof vb);
            vb.request.type = K_VBLANK_RELATIVE;
            vb.request.sequence = 1;
            if (k_WaitVBlank(K.fd, &vb) != 0)
                usleep(17000);                       /* no vblank wait: a 60 Hz period */
        }
        xtrace_end();

        pthread_mutex_lock(&K.lock);
        for (k = 0; k < K.nbo; k++)
            if (K.state[k] == 2)
                K.state[k] = 0;                      /* replaced on screen */
        K.state[b] = 2;
        memmove(&K.queue[0], &K.queue[1], (size_t)(K.nq - 1) * sizeof K.queue[0]);
        memmove(&K.fence[0], &K.fence[1], (size_t)(K.nq - 1) * sizeof K.fence[0]);
        K.nq--;
        K.flips++;
        pthread_cond_broadcast(&K.cv);
        pthread_mutex_unlock(&K.lock);
    }
    return NULL;
}

int nv2a_kms_init(void *window)
{
    const char *e = getenv("RECOMP_KMS_PRESENT"), *drv = SDL_GetCurrentVideoDriver();
    void *hd, *hg, *he;
    int missing = 0, i;
    KRes *res;
    KConn *conn = NULL;
    pthread_t th;
    struct {                            /* SDL_SysWMinfo, 2.0.16+ layout (SDL_SYSWM_KMSDRM 13) */
        uint8_t major, minor, patch;
        int subsystem;
        union { char pad[64]; struct { int dev_index, drm_fd; void *gbm_dev; } kmsdrm; } info;
    } wm;
    int (*getwm)(void *, void *);

    if (!(e && *e == '1') || !drv || (strcmp(drv, "KMSDRM") != 0 && strcmp(drv, "kmsdrm") != 0))
        return 0;
    if (!nv2a_gl_api_es) {
        fprintf(stderr, "  [KMS] needs the GLES renderer\n");
        return 0;
    }
    memset(&wm, 0, sizeof wm);
    wm.major = 2; wm.minor = 16;
    getwm = (int (*)(void *, void *))dlsym(RTLD_DEFAULT, "SDL_GetWindowWMInfo");
    if (!getwm || !getwm(window, &wm) || wm.subsystem != 13 || wm.info.kmsdrm.drm_fd < 0
        || !wm.info.kmsdrm.gbm_dev) {
        fprintf(stderr, "  [KMS] no KMSDRM handles from SDL (subsystem %d)\n", wm.subsystem);
        return 0;
    }
    K.fd = wm.info.kmsdrm.drm_fd;

    hd = dlopen("libdrm.so.2", RTLD_NOW | RTLD_GLOBAL);
    hg = dlopen("libgbm.so.1", RTLD_NOW | RTLD_GLOBAL);
    he = dlopen("libEGL.so", RTLD_NOW | RTLD_GLOBAL);
    k_GetResources = sym(hd, "drmModeGetResources", &missing);
    k_FreeResources = sym(hd, "drmModeFreeResources", &missing);
    k_GetConnector = sym(hd, "drmModeGetConnector", &missing);
    k_FreeConnector = sym(hd, "drmModeFreeConnector", &missing);
    k_GetEncoder = sym(hd, "drmModeGetEncoder", &missing);
    k_FreeEncoder = sym(hd, "drmModeFreeEncoder", &missing);
    k_AddFB = sym(hd, "drmModeAddFB", &missing);
    k_PageFlip = sym(hd, "drmModePageFlip", &missing);
    k_WaitVBlank = sym(hd, "drmWaitVBlank", &missing);
    k_bo_create = sym(hg, "gbm_bo_create", &missing);
    k_bo_stride = sym(hg, "gbm_bo_get_stride", &missing);
    k_bo_fd = sym(hg, "gbm_bo_get_fd", &missing);
    k_bo_handle = sym(hg, "gbm_bo_get_handle", &missing);
    k_eglGetCurrentDisplay = sym(he, "eglGetCurrentDisplay", &missing);
    k_eglGetProcAddress = sym(he, "eglGetProcAddress", &missing);
    if (missing)
        return 0;
    k_eglCreateImage = k_eglGetProcAddress("eglCreateImageKHR");
    k_eglCreateSync = k_eglGetProcAddress("eglCreateSyncKHR");
    k_eglClientWaitSync = k_eglGetProcAddress("eglClientWaitSyncKHR");
    k_eglDestroySync = k_eglGetProcAddress("eglDestroySyncKHR");
    k_glEGLImageTargetTexture2DOES = k_eglGetProcAddress("glEGLImageTargetTexture2DOES");
    k_glFlush = SDL_GL_GetProcAddress("glFlush");
    K.dpy = k_eglGetCurrentDisplay();
    if (!k_eglCreateImage || !k_eglCreateSync || !k_eglClientWaitSync || !k_eglDestroySync
        || !k_glEGLImageTargetTexture2DOES || !k_glFlush || !K.dpy) {
        fprintf(stderr, "  [KMS] EGL image/sync entry points or display missing\n");
        return 0;
    }

    /* The connected panel, its mode and the CRTC driving it. */
    res = k_GetResources(K.fd);
    if (!res) {
        fprintf(stderr, "  [KMS] drmModeGetResources failed\n");
        return 0;
    }
    for (i = 0; i < res->count_connectors && !conn; i++) {
        KConn *c = k_GetConnector(K.fd, res->connectors[i]);
        if (c && c->connection == 1 && c->count_modes > 0)
            conn = c;
        else if (c)
            k_FreeConnector(c);
    }
    if (conn) {
        KEnc *enc = conn->encoder_id ? k_GetEncoder(K.fd, conn->encoder_id) : NULL;
        if (enc && enc->crtc_id)
            K.crtc = enc->crtc_id;
        for (i = 0; !K.crtc && i < conn->count_encoders; i++) {
            KEnc *e2 = k_GetEncoder(K.fd, conn->encoders[i]);
            int j;
            for (j = 0; e2 && j < res->count_crtcs && !K.crtc; j++)
                if (e2->possible_crtcs & (1u << j))
                    K.crtc = res->crtcs[j];
            if (e2) k_FreeEncoder(e2);
        }
        if (enc) k_FreeEncoder(enc);
        K.w = conn->modes[0].hdisplay;
        K.h = conn->modes[0].vdisplay;
        k_FreeConnector(conn);
    }
    k_FreeResources(res);
    if (!K.crtc || K.w <= 0 || K.h <= 0) {
        fprintf(stderr, "  [KMS] no connected panel / CRTC\n");
        return 0;
    }

    /* Scanout bos, each a framebuffer and an FBO. reVC saw the blob fail the
     * third import; whatever imports is used, two at least. */
    {
        const char *nb = getenv("RECOMP_KMS_BOS");
        int want = nb ? atoi(nb) : 3;
        if (want < 2) want = 2;
        if (want > KMS_MAX_BOS) want = KMS_MAX_BOS;
        for (i = 0; i < want; i++) {
            void *bo = k_bo_create(wm.info.kmsdrm.gbm_dev, (uint32_t)K.w, (uint32_t)K.h,
                                   K_GBM_XRGB8888, K_GBM_BO_USE_SCANOUT | K_GBM_BO_USE_RENDERING);
            uint32_t stride, fb = 0;
            int fd;
            EGLImage img;
            GLuint tex = 0, fbo = 0;
            if (!bo)
                break;
            stride = k_bo_stride(bo);
            if (k_AddFB(K.fd, (uint32_t)K.w, (uint32_t)K.h, 24, 32, stride,
                        (uint32_t)k_bo_handle(bo), &fb) != 0) {
                fprintf(stderr, "  [KMS] drmModeAddFB: %s\n", strerror(errno));
                break;
            }
            fd = k_bo_fd(bo);
            {
                EGLint at[] = { K_EGL_WIDTH, K.w, K_EGL_HEIGHT, K.h,
                                K_EGL_LINUX_DRM_FOURCC, (EGLint)K_GBM_XRGB8888,
                                K_EGL_DMA_BUF_PLANE0_FD, fd, K_EGL_DMA_BUF_PLANE0_OFFSET, 0,
                                K_EGL_DMA_BUF_PLANE0_PITCH, (EGLint)stride, K_EGL_NONE };
                img = k_eglCreateImage(K.dpy, NULL, K_EGL_LINUX_DMA_BUF, NULL, at);
            }
            close(fd);
            if (!img) {
                fprintf(stderr, "  [KMS] dma-buf import of bo %d failed\n", i);
                break;
            }
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            k_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, img);
            glGenFramebuffers(1, &fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
                fprintf(stderr, "  [KMS] bo %d: FBO incomplete\n", i);
                glDeleteFramebuffers(1, &fbo);
                glDeleteTextures(1, &tex);
                break;
            }
            K.bo[i].bo = bo; K.bo[i].fb = fb; K.bo[i].tex = tex; K.bo[i].fbo = fbo;
            K.nbo = i + 1;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glBindTexture(GL_TEXTURE_2D, 0);
        while (glGetError() != GL_NO_ERROR) { }
    }
    if (K.nbo < 2) {
        fprintf(stderr, "  [KMS] %d scanout buffer(s): swapping instead\n", K.nbo);
        return 0;                       /* the made bos leak: a one-time few MB */
    }
    e = getenv("RECOMP_KMS_FLIPX");
    K.flipx = e && *e == '1';
    e = getenv("RECOMP_KMS_FLIPY");
    K.flipy = e && *e == '1';
    if (pthread_create(&th, NULL, flip_thread, NULL) != 0)
        return 0;
    pthread_detach(th);
    K.on = 1;
    fprintf(stderr, "  [KMS] presenting by page flip: %dx%d, crtc %u, %d buffers"
            " (RECOMP_KMS_PRESENT)\n", K.w, K.h, K.crtc, K.nbo);
    return 1;
}

void nv2a_kms_present(GLuint src, int sx0, int sy0, int sx1, int sy1,
                      int dx, int dy, int dw, int dh)
{
    int b = -1, k;
    uint64_t t0 = 0;
    EGLSync f;

    /* A free bo: none only while one is on screen and the rest are queued. */
    pthread_mutex_lock(&K.lock);
    for (;;) {
        for (k = 0; k < K.nbo && b < 0; k++)
            if (K.state[k] == 0)
                b = k;
        if (b >= 0)
            break;
        if (!t0) {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            t0 = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
            xtrace_begin("wait scanout buffer");
        }
        pthread_cond_wait(&K.cv, &K.lock);
    }
    pthread_mutex_unlock(&K.lock);
    if (t0) {
        struct timespec ts;
        xtrace_end();
        clock_gettime(CLOCK_MONOTONIC, &ts);
        K.gl_waits_ns += (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec - t0;
    }

    /* Surfaces hold rows top-first; the bo's orientation on the panel is
     * the flags' business (see the top of the file). */
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, K.bo[b].fbo);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(1, 1, 1, 1);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (src) {
        /* The letterbox is centred, so mirroring the destination rectangle
         * in place is mirroring it on the panel. */
        int x0 = K.flipx ? dx + dw : dx, x1 = K.flipx ? dx : dx + dw;
        int y0 = K.flipy ? dy + dh : dy, y1 = K.flipy ? dy : dy + dh;
        glBindFramebuffer(GL_READ_FRAMEBUFFER, src);
        glBlitFramebuffer(sx0, sy0, sx1, sy1, x0, y0, x1, y1,
                          GL_COLOR_BUFFER_BIT, GL_LINEAR);
    }
    f = k_eglCreateSync(K.dpy, K_EGL_SYNC_FENCE, NULL);
    k_glFlush();                        /* the fence must reach the GPU */

    pthread_mutex_lock(&K.lock);
    K.state[b] = 1;
    K.queue[K.nq] = b;
    K.fence[K.nq] = f;
    K.nq++;
    pthread_cond_broadcast(&K.cv);
    pthread_mutex_unlock(&K.lock);
}
#endif /* !_WIN32 && !__SWITCH__ */
