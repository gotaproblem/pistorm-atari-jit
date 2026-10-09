/* SPDX-License-Identifier: MIT
 *
 * stbox_gl.h - the few EGL / GLES 3.1 declarations PSGFX-in-STBOX uses,
 * resolved at run time (dlopen libEGL.so.1 + eglGetProcAddress). No build
 * dependency on Mesa headers or libraries: a Pi without libegl1/libgles2
 * builds the emulator unchanged and just never enables the GPU path.
 * Values are the Khronos registry's (checked against Mesa's headers by
 * tools/stbox-harness/glcheck.c).
 */
#ifndef PISTORM_STBOX_GL_H
#define PISTORM_STBOX_GL_H
#include <stdint.h>
#include <stddef.h>

typedef unsigned int GLenum, GLuint, GLbitfield;
typedef int GLint, GLsizei;
typedef float GLfloat;
typedef char GLchar;
typedef unsigned char GLboolean;
typedef ptrdiff_t GLsizeiptr;
typedef void *EGLDisplay, *EGLContext, *EGLSurface, *EGLConfig;
typedef int32_t EGLint;
typedef unsigned int EGLBoolean, EGLenum;

#define GL_VERTEX_SHADER          0x8B31
#define GL_FRAGMENT_SHADER        0x8B30
#define GL_COMPILE_STATUS         0x8B81
#define GL_LINK_STATUS            0x8B82
#define GL_ARRAY_BUFFER           0x8892
#define GL_STATIC_DRAW            0x88E4
#define GL_STREAM_DRAW            0x88E0
#define GL_TEXTURE_2D             0x0DE1
#define GL_RGBA                   0x1908
#define GL_UNSIGNED_BYTE          0x1401
#define GL_FLOAT                  0x1406
#define GL_TEXTURE_MIN_FILTER     0x2801
#define GL_TEXTURE_MAG_FILTER     0x2800
#define GL_TEXTURE_WRAP_S         0x2802
#define GL_TEXTURE_WRAP_T         0x2803
#define GL_CLAMP_TO_EDGE          0x812F
#define GL_NEAREST                0x2600
#define GL_LINEAR                 0x2601
#define GL_TEXTURE0               0x84C0
#define GL_RENDERBUFFER           0x8D41
#define GL_FRAMEBUFFER            0x8D40
#define GL_READ_FRAMEBUFFER       0x8CA8
#define GL_DRAW_FRAMEBUFFER       0x8CA9
#define GL_COLOR_ATTACHMENT0      0x8CE0
#define GL_DEPTH_ATTACHMENT       0x8D00
#define GL_RGBA8                  0x8058
#define GL_DEPTH_COMPONENT24      0x81A6
#define GL_FRAMEBUFFER_COMPLETE   0x8CD5
#define GL_COLOR_BUFFER_BIT       0x4000
#define GL_DEPTH_BUFFER_BIT       0x0100
#define GL_DEPTH_TEST             0x0B71
#define GL_BLEND                  0x0BE2
#define GL_SCISSOR_TEST           0x0C11
#define GL_CULL_FACE              0x0B44
#define GL_LESS                   0x0201
#define GL_SRC_ALPHA              0x0302
#define GL_ONE_MINUS_SRC_ALPHA    0x0303
#define GL_TRIANGLES              0x0004
#define GL_TRIANGLE_STRIP         0x0005
#define GL_PACK_ROW_LENGTH        0x0D02
#define GL_PACK_ALIGNMENT         0x0D05
#define GL_UNPACK_ALIGNMENT       0x0CF5
#define GL_NO_ERROR               0
#define GL_RENDERER               0x1F01

#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#define EGL_OPENGL_ES_API         0x30A0
#define EGL_CONTEXT_MAJOR_VERSION 0x3098
#define EGL_CONTEXT_MINOR_VERSION 0x30FB
#define EGL_NONE                  0x3038
#define EGL_EXTENSIONS            0x3055

/* X-macro: every GL entry point used, as (return, name, args) */
#define STBOX_GL_FUNCS(X) \
    X(GLuint, glCreateShader, (GLenum)) \
    X(void, glShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *)) \
    X(void, glCompileShader, (GLuint)) \
    X(void, glGetShaderiv, (GLuint, GLenum, GLint *)) \
    X(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
    X(GLuint, glCreateProgram, (void)) \
    X(void, glAttachShader, (GLuint, GLuint)) \
    X(void, glLinkProgram, (GLuint)) \
    X(void, glGetProgramiv, (GLuint, GLenum, GLint *)) \
    X(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
    X(void, glUseProgram, (GLuint)) \
    X(GLint, glGetUniformLocation, (GLuint, const GLchar *)) \
    X(void, glUniform1i, (GLint, GLint)) \
    X(void, glUniform1f, (GLint, GLfloat)) \
    X(void, glUniform2f, (GLint, GLfloat, GLfloat)) \
    X(void, glUniform3f, (GLint, GLfloat, GLfloat, GLfloat)) \
    X(void, glUniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(void, glGenBuffers, (GLsizei, GLuint *)) \
    X(void, glBindBuffer, (GLenum, GLuint)) \
    X(void, glBufferData, (GLenum, GLsizeiptr, const void *, GLenum)) \
    X(void, glGenTextures, (GLsizei, GLuint *)) \
    X(void, glBindTexture, (GLenum, GLuint)) \
    X(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)) \
    X(void, glTexParameteri, (GLenum, GLenum, GLint)) \
    X(void, glActiveTexture, (GLenum)) \
    X(void, glGenRenderbuffers, (GLsizei, GLuint *)) \
    X(void, glBindRenderbuffer, (GLenum, GLuint)) \
    X(void, glRenderbufferStorageMultisample, (GLenum, GLsizei, GLenum, GLsizei, GLsizei)) \
    X(void, glRenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei)) \
    X(void, glGenFramebuffers, (GLsizei, GLuint *)) \
    X(void, glBindFramebuffer, (GLenum, GLuint)) \
    X(void, glFramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint)) \
    X(GLenum, glCheckFramebufferStatus, (GLenum)) \
    X(void, glBlitFramebuffer, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum)) \
    X(void, glViewport, (GLint, GLint, GLsizei, GLsizei)) \
    X(void, glEnable, (GLenum)) \
    X(void, glDisable, (GLenum)) \
    X(void, glClearColor, (GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(void, glClear, (GLbitfield)) \
    X(void, glScissor, (GLint, GLint, GLsizei, GLsizei)) \
    X(void, glDepthFunc, (GLenum)) \
    X(void, glBlendFunc, (GLenum, GLenum)) \
    X(void, glEnableVertexAttribArray, (GLuint)) \
    X(void, glDisableVertexAttribArray, (GLuint)) \
    X(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)) \
    X(void, glDrawArrays, (GLenum, GLint, GLsizei)) \
    X(void, glReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *)) \
    X(void, glPixelStorei, (GLenum, GLint)) \
    X(GLenum, glGetError, (void)) \
    X(const unsigned char *, glGetString, (GLenum))

#define STBOX_EGL_FUNCS(X) \
    X(void *, eglGetProcAddress, (const char *)) \
    X(EGLBoolean, eglInitialize, (EGLDisplay, EGLint *, EGLint *)) \
    X(EGLBoolean, eglBindAPI, (EGLenum)) \
    X(EGLContext, eglCreateContext, (EGLDisplay, EGLConfig, EGLContext, const EGLint *)) \
    X(EGLBoolean, eglMakeCurrent, (EGLDisplay, EGLSurface, EGLSurface, EGLContext)) \
    X(EGLBoolean, eglDestroyContext, (EGLDisplay, EGLContext)) \
    X(EGLBoolean, eglTerminate, (EGLDisplay)) \
    X(EGLint, eglGetError, (void)) \
    X(const char *, eglQueryString, (EGLDisplay, EGLint))

#endif
