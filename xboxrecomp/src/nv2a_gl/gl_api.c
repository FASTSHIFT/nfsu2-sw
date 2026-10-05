/* gl_api.c -- fill the entry-point table declared in gl_api.h. */
#include "gl_api.h"

#include <stdio.h>

#define NV2A_GL_DEFINE(ret, name, args) PFN_##name p_##name;
NV2A_GL_FUNCS(NV2A_GL_DEFINE)
#undef NV2A_GL_DEFINE

uint32_t nv2a_gl_calls[NV2A_GLID_COUNT];
#define NV2A_GL_NAME(ret, name, args) #name,
const char *const nv2a_gl_names[NV2A_GLID_COUNT] = { NV2A_GL_FUNCS(NV2A_GL_NAME) };
#undef NV2A_GL_NAME

int nv2a_gl_api_es;
PFN_glClearDepth  p_glClearDepth;
PFN_glClearDepthf p_glClearDepthf;

int nv2a_gl_load(void *(*getproc)(const char *name))
{
    int missing = 0;

#define NV2A_GL_RESOLVE(ret, name, args) \
    p_##name = (PFN_##name)getproc(#name); \
    if (!p_##name) { \
        fprintf(stderr, "  [GL] missing entry point %s\n", #name); \
        missing++; \
    }
    NV2A_GL_FUNCS(NV2A_GL_RESOLVE)
#undef NV2A_GL_RESOLVE

    /* The one call whose signature differs between desktop GL and ES. */
    if (nv2a_gl_api_es) {
        p_glClearDepthf = (PFN_glClearDepthf)getproc("glClearDepthf");
        if (!p_glClearDepthf) {
            fprintf(stderr, "  [GL] missing entry point glClearDepthf\n");
            missing++;
        }
    } else {
        p_glClearDepth = (PFN_glClearDepth)getproc("glClearDepth");
        if (!p_glClearDepth) {
            fprintf(stderr, "  [GL] missing entry point glClearDepth\n");
            missing++;
        }
    }
    return missing;
}
