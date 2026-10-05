/*
 * gl_api.h -- the slice of OpenGL 3.3 core the NV2A renderer uses.
 *
 * Declared here rather than taken from a loader library: the renderer has to
 * build against Linux Mesa, Windows drivers and the Switch's Mesa port alike,
 * and a table of ~70 entry points filled from SDL_GL_GetProcAddress is less
 * to carry than a generated loader per platform.
 */
#ifndef NV2A_GL_API_H
#define NV2A_GL_API_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#  define GLAPIENTRY __stdcall
#else
#  define GLAPIENTRY
#endif

typedef unsigned int  GLenum;
typedef unsigned char GLboolean;
typedef unsigned int  GLbitfield;
typedef int           GLint;
typedef int           GLsizei;
typedef unsigned int  GLuint;
typedef float         GLfloat;
typedef double        GLdouble;
typedef char          GLchar;
typedef unsigned char GLubyte;
typedef ptrdiff_t     GLsizeiptr;
typedef ptrdiff_t     GLintptr;
typedef uint64_t      GLuint64;
typedef struct __GLsync *GLsync;

#define GL_MAP_PERSISTENT_BIT             0x0040
#define GL_MAP_COHERENT_BIT               0x0080
#define GL_SYNC_GPU_COMMANDS_COMPLETE     0x9117
#define GL_SYNC_FLUSH_COMMANDS_BIT        0x00000001
#define GL_TIMEOUT_EXPIRED                0x911B
#define GL_WAIT_FAILED                    0x911D

#define GL_FALSE                          0
#define GL_NO_ERROR                       0
#define GL_MAP_WRITE_BIT                  0x0002
#define GL_MAP_INVALIDATE_RANGE_BIT       0x0004
#define GL_MAP_UNSYNCHRONIZED_BIT         0x0020
#define GL_TRUE                           1
#define GL_NONE                           0
#define GL_ZERO                           0
#define GL_ONE                            1
#define GL_TRIANGLES                      0x0004
#define GL_NEVER                          0x0200
#define GL_ALWAYS                         0x0207
#define GL_FRONT                          0x0404
#define GL_BACK                           0x0405
#define GL_FRONT_AND_BACK                 0x0408
#define GL_CW                             0x0900
#define GL_CCW                            0x0901
#define GL_CULL_FACE                      0x0B44
#define GL_DEPTH_TEST                     0x0B71
#define GL_STENCIL_TEST                   0x0B90
#define GL_BLEND                          0x0BE2
#define GL_SCISSOR_TEST                   0x0C11
#define GL_VIEWPORT                       0x0BA2
#define GL_UNPACK_ALIGNMENT               0x0CF5
#define GL_UNPACK_ROW_LENGTH              0x0CF2
#define GL_PACK_ALIGNMENT                 0x0D05
#define GL_TEXTURE_2D                     0x0DE1
#define GL_UNSIGNED_BYTE                  0x1401
#define GL_UNSIGNED_SHORT                 0x1403
#define GL_UNSIGNED_INT                   0x1405
#define GL_SHORT                          0x1402
#define GL_FLOAT                          0x1406
#define GL_RGBA                           0x1908
#define GL_KEEP                           0x1E00
#define GL_VENDOR                         0x1F00
#define GL_RENDERER                       0x1F01
#define GL_VERSION                        0x1F02
#define GL_NEAREST                        0x2600
#define GL_LINEAR                         0x2601
#define GL_LINEAR_MIPMAP_LINEAR           0x2703
#define GL_TEXTURE_MAG_FILTER             0x2800
#define GL_TEXTURE_MIN_FILTER             0x2801
#define GL_TEXTURE_WRAP_S                 0x2802
#define GL_TEXTURE_WRAP_T                 0x2803
#define GL_REPEAT                         0x2901
#define GL_COLOR_BUFFER_BIT               0x00004000
#define GL_DEPTH_BUFFER_BIT               0x00000100
#define GL_STENCIL_BUFFER_BIT             0x00000400
#define GL_FUNC_ADD                       0x8006
#define GL_BGRA                           0x80E1
#define GL_CLAMP_TO_BORDER                0x812D
#define GL_CLAMP_TO_EDGE                  0x812F
#define GL_MIRRORED_REPEAT                0x8370
#define GL_TEXTURE0                       0x84C0
#define GL_DEPTH_STENCIL                  0x84F9
#define GL_UNSIGNED_INT_8_8_8_8_REV       0x8367
#define GL_ARRAY_BUFFER                   0x8892
#define GL_ELEMENT_ARRAY_BUFFER           0x8893
#define GL_STREAM_DRAW                    0x88E0
#define GL_DEPTH24_STENCIL8               0x88F0
#define GL_FRAGMENT_SHADER                0x8B30
#define GL_VERTEX_SHADER                  0x8B31
#define GL_COMPILE_STATUS                 0x8B81
#define GL_LINK_STATUS                    0x8B82
#define GL_INFO_LOG_LENGTH                0x8B84
#define GL_READ_FRAMEBUFFER               0x8CA8
#define GL_DRAW_FRAMEBUFFER               0x8CA9
#define GL_FRAMEBUFFER_COMPLETE           0x8CD5
#define GL_COLOR_ATTACHMENT0              0x8CE0
#define GL_DEPTH_STENCIL_ATTACHMENT       0x821A
#define GL_FRAMEBUFFER                    0x8D40
#define GL_RENDERBUFFER                   0x8D41
#define GL_RGBA8                          0x8058
#define GL_INCR                           0x1E02
#define GL_DECR                           0x1E03
#define GL_INVERT                         0x150A
#define GL_REPLACE                        0x1E01

/* X(return type, name, parameter list) */
#define NV2A_GL_FUNCS(X) \
    X(const GLubyte *, glGetString, (GLenum)) \
    X(GLenum, glGetError, (void)) \
    X(void, glViewport, (GLint, GLint, GLsizei, GLsizei)) \
    X(void, glScissor, (GLint, GLint, GLsizei, GLsizei)) \
    X(void, glClearColor, (GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(void, glGetIntegerv, (GLenum, GLint *)) \
    X(void, glClearStencil, (GLint)) \
    X(void, glClear, (GLbitfield)) \
    X(void, glEnable, (GLenum)) \
    X(void, glDisable, (GLenum)) \
    X(void, glBlendFunc, (GLenum, GLenum)) \
    X(void, glBlendEquation, (GLenum)) \
    X(void, glBlendColor, (GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(void, glDepthFunc, (GLenum)) \
    X(void, glDepthMask, (GLboolean)) \
    X(void, glColorMask, (GLboolean, GLboolean, GLboolean, GLboolean)) \
    X(void, glStencilFunc, (GLenum, GLint, GLuint)) \
    X(void, glStencilOp, (GLenum, GLenum, GLenum)) \
    X(void, glStencilMask, (GLuint)) \
    X(void, glCullFace, (GLenum)) \
    X(void, glFrontFace, (GLenum)) \
    X(void, glPixelStorei, (GLenum, GLint)) \
    X(void, glReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *)) \
    X(void, glFinish, (void)) \
    X(void, glGenTextures, (GLsizei, GLuint *)) \
    X(void, glDeleteTextures, (GLsizei, const GLuint *)) \
    X(void, glBindTexture, (GLenum, GLuint)) \
    X(void, glActiveTexture, (GLenum)) \
    X(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)) \
    X(void, glTexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *)) \
    X(void, glCompressedTexImage2D, (GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void *)) \
    X(void, glTexParameteri, (GLenum, GLenum, GLint)) \
    X(void, glGenFramebuffers, (GLsizei, GLuint *)) \
    X(void, glDeleteFramebuffers, (GLsizei, const GLuint *)) \
    X(void, glBindFramebuffer, (GLenum, GLuint)) \
    X(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint)) \
    X(void, glFramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint)) \
    X(GLenum, glCheckFramebufferStatus, (GLenum)) \
    X(void, glBlitFramebuffer, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum)) \
    X(void, glGenRenderbuffers, (GLsizei, GLuint *)) \
    X(void, glDeleteRenderbuffers, (GLsizei, const GLuint *)) \
    X(void, glBindRenderbuffer, (GLenum, GLuint)) \
    X(void, glRenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei)) \
    X(GLuint, glCreateShader, (GLenum)) \
    X(void, glDeleteShader, (GLuint)) \
    X(void, glDeleteProgram, (GLuint)) \
    X(void, glShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *)) \
    X(void, glCompileShader, (GLuint)) \
    X(void, glGetShaderiv, (GLuint, GLenum, GLint *)) \
    X(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
    X(GLuint, glCreateProgram, (void)) \
    X(void, glAttachShader, (GLuint, GLuint)) \
    X(void, glBindAttribLocation, (GLuint, GLuint, const GLchar *)) \
    X(void, glLinkProgram, (GLuint)) \
    X(void, glGetProgramiv, (GLuint, GLenum, GLint *)) \
    X(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
    X(void, glUseProgram, (GLuint)) \
    X(GLint, glGetUniformLocation, (GLuint, const GLchar *)) \
    X(void, glUniform1i, (GLint, GLint)) \
    X(void, glUniform1iv, (GLint, GLsizei, const GLint *)) \
    X(void, glUniform1f, (GLint, GLfloat)) \
    X(void, glUniform2fv, (GLint, GLsizei, const GLfloat *)) \
    X(void, glUniform4fv, (GLint, GLsizei, const GLfloat *)) \
    X(void, glGenVertexArrays, (GLsizei, GLuint *)) \
    X(void, glBindVertexArray, (GLuint)) \
    X(void, glGenBuffers, (GLsizei, GLuint *)) \
    X(void, glBindBuffer, (GLenum, GLuint)) \
    X(void, glBufferData, (GLenum, GLsizeiptr, const void *, GLenum)) \
X(void, glBufferSubData, (GLenum, GLintptr, GLsizeiptr, const void *)) \
X(void *, glMapBufferRange, (GLenum, GLintptr, GLsizeiptr, GLbitfield)) \
X(GLboolean, glUnmapBuffer, (GLenum)) \
    X(GLsync, glFenceSync, (GLenum, GLbitfield)) \
    X(GLenum, glClientWaitSync, (GLsync, GLbitfield, GLuint64)) \
    X(void, glDeleteSync, (GLsync)) \
    X(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)) \
    X(void, glEnableVertexAttribArray, (GLuint)) \
    X(void, glDisableVertexAttribArray, (GLuint)) \
    X(void, glVertexAttrib4f, (GLuint, GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(void, glDrawElements, (GLenum, GLsizei, GLenum, const void *)) \
    X(void, glDrawArrays, (GLenum, GLint, GLsizei))

#define NV2A_GL_DECLARE(ret, name, args) \
    typedef ret (GLAPIENTRY *PFN_##name) args; extern PFN_##name p_##name;
NV2A_GL_FUNCS(NV2A_GL_DECLARE)
#undef NV2A_GL_DECLARE

/* Calls per entry point (RECOMP_GL_CALL_STATS, nv2a_gl.c): every call through
 * the names below bumps its counter. One non-atomic add -- only one thread at
 * a time owns the context -- so it stays on. */
#define NV2A_GL_ID(ret, name, args) NV2A_GLID_##name,
enum { NV2A_GL_FUNCS(NV2A_GL_ID) NV2A_GLID_COUNT };
#undef NV2A_GL_ID
extern uint32_t nv2a_gl_calls[NV2A_GLID_COUNT];
extern const char *const nv2a_gl_names[NV2A_GLID_COUNT];
/* A function, not a comma expression: two counted calls in one expression
 * (a printf of two glGetString) stay sequenced. */
static inline void nv2a_gl_count(int id) { nv2a_gl_calls[id]++; }
#define NV2A_GLCALL(name) (nv2a_gl_count(NV2A_GLID_##name), p_##name)

/* Resolve every entry point. Returns the number that could not be found
 * (0 on success); `getproc` is SDL_GL_GetProcAddress or equivalent. */
int nv2a_gl_load(void *(*getproc)(const char *name));

/* OpenGL ES 3.x instead of desktop GL 3.3 core (set before nv2a_gl_load).
 * ES has no double-precision depth clear, only glClearDepthf; the call
 * below picks whichever the context has. */
extern int nv2a_gl_api_es;
typedef void (GLAPIENTRY *PFN_glClearDepth)(GLdouble);
typedef void (GLAPIENTRY *PFN_glClearDepthf)(GLfloat);
extern PFN_glClearDepth  p_glClearDepth;
extern PFN_glClearDepthf p_glClearDepthf;
static inline void nv2a_glClearDepth(double d)
{
    if (nv2a_gl_api_es)
        p_glClearDepthf((GLfloat)d);
    else
        p_glClearDepth(d);
}

#define GL_EXTENSIONS                     0x1F03
#define GL_TEXTURE_SWIZZLE_R              0x8E42
#define GL_TEXTURE_SWIZZLE_B              0x8E44
#define GL_RED                            0x1903
#define GL_BLUE                           0x1905

/* The renderer calls through these names. */
#define glGetString              NV2A_GLCALL(glGetString)
#define glGetError               NV2A_GLCALL(glGetError)
#define glViewport               NV2A_GLCALL(glViewport)
#define glScissor                NV2A_GLCALL(glScissor)
#define glClearColor             NV2A_GLCALL(glClearColor)
#define glGetIntegerv            NV2A_GLCALL(glGetIntegerv)
#define glClearDepth             nv2a_glClearDepth
#define glClearStencil           NV2A_GLCALL(glClearStencil)
#define glClear                  NV2A_GLCALL(glClear)
#define glEnable                 NV2A_GLCALL(glEnable)
#define glDisable                NV2A_GLCALL(glDisable)
#define glBlendFunc              NV2A_GLCALL(glBlendFunc)
#define glBlendEquation          NV2A_GLCALL(glBlendEquation)
#define glBlendColor             NV2A_GLCALL(glBlendColor)
#define glDepthFunc              NV2A_GLCALL(glDepthFunc)
#define glDepthMask              NV2A_GLCALL(glDepthMask)
#define glColorMask              NV2A_GLCALL(glColorMask)
#define glStencilFunc            NV2A_GLCALL(glStencilFunc)
#define glStencilOp              NV2A_GLCALL(glStencilOp)
#define glStencilMask            NV2A_GLCALL(glStencilMask)
#define glCullFace               NV2A_GLCALL(glCullFace)
#define glFrontFace              NV2A_GLCALL(glFrontFace)
#define glPixelStorei            NV2A_GLCALL(glPixelStorei)
#define glReadPixels             NV2A_GLCALL(glReadPixels)
#define glFinish                 NV2A_GLCALL(glFinish)
#define glGenTextures            NV2A_GLCALL(glGenTextures)
#define glDeleteTextures         NV2A_GLCALL(glDeleteTextures)
#define glBindTexture            NV2A_GLCALL(glBindTexture)
#define glActiveTexture          NV2A_GLCALL(glActiveTexture)
#define glTexImage2D             NV2A_GLCALL(glTexImage2D)
#define glCompressedTexImage2D   NV2A_GLCALL(glCompressedTexImage2D)
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83F2
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#define glTexSubImage2D          NV2A_GLCALL(glTexSubImage2D)
#define glTexParameteri          NV2A_GLCALL(glTexParameteri)
#define glGenFramebuffers        NV2A_GLCALL(glGenFramebuffers)
#define glDeleteFramebuffers     NV2A_GLCALL(glDeleteFramebuffers)
#define glBindFramebuffer        NV2A_GLCALL(glBindFramebuffer)
#define glFramebufferTexture2D   NV2A_GLCALL(glFramebufferTexture2D)
#define glFramebufferRenderbuffer NV2A_GLCALL(glFramebufferRenderbuffer)
#define glCheckFramebufferStatus NV2A_GLCALL(glCheckFramebufferStatus)
#define glBlitFramebuffer        NV2A_GLCALL(glBlitFramebuffer)
#define glGenRenderbuffers       NV2A_GLCALL(glGenRenderbuffers)
#define glDeleteRenderbuffers    NV2A_GLCALL(glDeleteRenderbuffers)
#define glBindRenderbuffer       NV2A_GLCALL(glBindRenderbuffer)
#define glRenderbufferStorage    NV2A_GLCALL(glRenderbufferStorage)
#define glCreateShader           NV2A_GLCALL(glCreateShader)
#define glDeleteShader           NV2A_GLCALL(glDeleteShader)
#define glDeleteProgram          NV2A_GLCALL(glDeleteProgram)
#define glShaderSource           NV2A_GLCALL(glShaderSource)
#define glCompileShader          NV2A_GLCALL(glCompileShader)
#define glGetShaderiv            NV2A_GLCALL(glGetShaderiv)
#define glGetShaderInfoLog       NV2A_GLCALL(glGetShaderInfoLog)
#define glCreateProgram          NV2A_GLCALL(glCreateProgram)
#define glAttachShader           NV2A_GLCALL(glAttachShader)
#define glBindAttribLocation     NV2A_GLCALL(glBindAttribLocation)
#define glLinkProgram            NV2A_GLCALL(glLinkProgram)
#define glGetProgramiv           NV2A_GLCALL(glGetProgramiv)
#define glGetProgramInfoLog      NV2A_GLCALL(glGetProgramInfoLog)
#define glUseProgram             NV2A_GLCALL(glUseProgram)
#define glGetUniformLocation     NV2A_GLCALL(glGetUniformLocation)
#define glUniform1i              NV2A_GLCALL(glUniform1i)
#define glUniform1iv             NV2A_GLCALL(glUniform1iv)
#define glUniform1f              NV2A_GLCALL(glUniform1f)
#define glUniform2fv             NV2A_GLCALL(glUniform2fv)
#define glUniform4fv             NV2A_GLCALL(glUniform4fv)
#define glGenVertexArrays        NV2A_GLCALL(glGenVertexArrays)
#define glBindVertexArray        NV2A_GLCALL(glBindVertexArray)
#define glGenBuffers             NV2A_GLCALL(glGenBuffers)
#define glBindBuffer             NV2A_GLCALL(glBindBuffer)
#define glBufferData             NV2A_GLCALL(glBufferData)
#define glBufferSubData          NV2A_GLCALL(glBufferSubData)
#define glMapBufferRange         NV2A_GLCALL(glMapBufferRange)
#define glUnmapBuffer            NV2A_GLCALL(glUnmapBuffer)
#define glFenceSync              NV2A_GLCALL(glFenceSync)
#define glClientWaitSync         NV2A_GLCALL(glClientWaitSync)
#define glDeleteSync             NV2A_GLCALL(glDeleteSync)
#define glVertexAttribPointer    NV2A_GLCALL(glVertexAttribPointer)
#define glEnableVertexAttribArray NV2A_GLCALL(glEnableVertexAttribArray)
#define glDisableVertexAttribArray NV2A_GLCALL(glDisableVertexAttribArray)
#define glVertexAttrib4f         NV2A_GLCALL(glVertexAttrib4f)
#define glDrawElements           NV2A_GLCALL(glDrawElements)
#define glDrawArrays             NV2A_GLCALL(glDrawArrays)

#endif /* NV2A_GL_API_H */
