/*
 * kms_present.h -- present through KMS page flips instead of eglSwapBuffers
 * (Linux KMSDRM, RECOMP_KMS_PRESENT=1). See kms_present.c.
 */
#ifndef NV2A_KMS_PRESENT_H
#define NV2A_KMS_PRESENT_H

#include "gl_api.h"

/* Set up on the GL thread, its context current. Returns 1 if frames now go
 * out through nv2a_kms_present (never call SDL_GL_SwapWindow after that). */
int nv2a_kms_init(void *sdl_window);
int nv2a_kms_active(void);
void nv2a_kms_mode(int *w, int *h);

/* Copy src (an FBO, or 0 for black) rect (sx0,sy0)-(sx1,sy1) to the panel
 * rect (dx,dy,dw,dh) -- panel rows top-first -- and queue it for the next
 * vblank. Waits only when every scanout buffer is still in use. */
void nv2a_kms_present(GLuint src_fbo, int sx0, int sy0, int sx1, int sy1,
                      int dx, int dy, int dw, int dh);

#endif
