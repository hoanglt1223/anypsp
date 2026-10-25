#pragma once

// Desktop OpenGL 3.3 core entry points for the GE's GL backend (ge_gl.cpp),
// which is written against OpenGL ES 3.0. Every function it calls exists in
// GL 3.3 core; they are loaded at run time because Windows' opengl32 only
// exports GL 1.1. Including this header redirects the gl* names to the loaded
// pointers.

#define SDL_OPENGL_1_NO_PROTOTYPES
#define SDL_OPENGL_1_FUNCTION_TYPEDEFS
#include <SDL3/SDL_opengl.h>

#define ANYPSP_GL_FUNCTIONS(X)                                                                                         \
    X(ACTIVETEXTURE, glActiveTexture)                                                                                  \
    X(ATTACHSHADER, glAttachShader)                                                                                    \
    X(BINDBUFFER, glBindBuffer)                                                                                        \
    X(BINDFRAMEBUFFER, glBindFramebuffer)                                                                              \
    X(BINDRENDERBUFFER, glBindRenderbuffer)                                                                            \
    X(BINDSAMPLER, glBindSampler)                                                                                      \
    X(BINDTEXTURE, glBindTexture)                                                                                      \
    X(BINDVERTEXARRAY, glBindVertexArray)                                                                              \
    X(BLENDCOLOR, glBlendColor)                                                                                        \
    X(BLENDEQUATION, glBlendEquation)                                                                                  \
    X(BLENDFUNC, glBlendFunc)                                                                                          \
    X(BLITFRAMEBUFFER, glBlitFramebuffer)                                                                              \
    X(BUFFERDATA, glBufferData)                                                                                        \
    X(CHECKFRAMEBUFFERSTATUS, glCheckFramebufferStatus)                                                                \
    X(CLEAR, glClear)                                                                                                  \
    X(CLEARCOLOR, glClearColor)                                                                                        \
    X(CLEARDEPTH, glClearDepth)                                                                                        \
    X(CLEARSTENCIL, glClearStencil)                                                                                    \
    X(COLORMASK, glColorMask)                                                                                          \
    X(COMPILESHADER, glCompileShader)                                                                                  \
    X(COPYTEXSUBIMAGE2D, glCopyTexSubImage2D)                                                                          \
    X(CREATEPROGRAM, glCreateProgram)                                                                                  \
    X(CREATESHADER, glCreateShader)                                                                                    \
    X(DELETEBUFFERS, glDeleteBuffers)                                                                                  \
    X(DELETEFRAMEBUFFERS, glDeleteFramebuffers)                                                                        \
    X(DELETEPROGRAM, glDeleteProgram)                                                                                  \
    X(DELETERENDERBUFFERS, glDeleteRenderbuffers)                                                                      \
    X(DELETESAMPLERS, glDeleteSamplers)                                                                                \
    X(DELETESHADER, glDeleteShader)                                                                                    \
    X(DELETETEXTURES, glDeleteTextures)                                                                                \
    X(DELETEVERTEXARRAYS, glDeleteVertexArrays)                                                                        \
    X(DEPTHFUNC, glDepthFunc)                                                                                          \
    X(DEPTHMASK, glDepthMask)                                                                                          \
    X(DISABLE, glDisable)                                                                                              \
    X(DRAWARRAYS, glDrawArrays)                                                                                        \
    X(DRAWBUFFERS, glDrawBuffers)                                                                                      \
    X(DRAWELEMENTS, glDrawElements)                                                                                    \
    X(ENABLE, glEnable)                                                                                                \
    X(ENABLEVERTEXATTRIBARRAY, glEnableVertexAttribArray)                                                              \
    X(FINISH, glFinish)                                                                                                \
    X(FRAMEBUFFERRENDERBUFFER, glFramebufferRenderbuffer)                                                              \
    X(FRAMEBUFFERTEXTURE2D, glFramebufferTexture2D)                                                                    \
    X(GENBUFFERS, glGenBuffers)                                                                                        \
    X(GENFRAMEBUFFERS, glGenFramebuffers)                                                                              \
    X(GENRENDERBUFFERS, glGenRenderbuffers)                                                                            \
    X(GENSAMPLERS, glGenSamplers)                                                                                      \
    X(GENTEXTURES, glGenTextures)                                                                                      \
    X(GENVERTEXARRAYS, glGenVertexArrays)                                                                              \
    X(GETERROR, glGetError)                                                                                            \
    X(GETINTEGERV, glGetIntegerv)                                                                                      \
    X(GETPROGRAMINFOLOG, glGetProgramInfoLog)                                                                          \
    X(GETPROGRAMIV, glGetProgramiv)                                                                                    \
    X(GETSHADERINFOLOG, glGetShaderInfoLog)                                                                            \
    X(GETSHADERIV, glGetShaderiv)                                                                                      \
    X(GETSTRING, glGetString)                                                                                          \
    X(GETUNIFORMLOCATION, glGetUniformLocation)                                                                        \
    X(LINKPROGRAM, glLinkProgram)                                                                                      \
    X(PIXELSTOREI, glPixelStorei)                                                                                      \
    X(READBUFFER, glReadBuffer)                                                                                        \
    X(READPIXELS, glReadPixels)                                                                                        \
    X(RENDERBUFFERSTORAGE, glRenderbufferStorage)                                                                      \
    X(SAMPLERPARAMETERI, glSamplerParameteri)                                                                          \
    X(SCISSOR, glScissor)                                                                                              \
    X(SHADERSOURCE, glShaderSource)                                                                                    \
    X(STENCILFUNC, glStencilFunc)                                                                                      \
    X(STENCILMASK, glStencilMask)                                                                                      \
    X(STENCILOP, glStencilOp)                                                                                          \
    X(TEXIMAGE2D, glTexImage2D)                                                                                        \
    X(TEXPARAMETERI, glTexParameteri)                                                                                  \
    X(TEXSUBIMAGE2D, glTexSubImage2D)                                                                                  \
    X(UNIFORM1F, glUniform1f)                                                                                          \
    X(UNIFORM1I, glUniform1i)                                                                                          \
    X(UNIFORM2F, glUniform2f)                                                                                          \
    X(UNIFORM2I, glUniform2i)                                                                                          \
    X(UNIFORM3F, glUniform3f)                                                                                          \
    X(UNIFORM4F, glUniform4f)                                                                                          \
    X(UNIFORM4FV, glUniform4fv)                                                                                        \
    X(UNIFORM4I, glUniform4i)                                                                                          \
    X(UNIFORM4IV, glUniform4iv)                                                                                        \
    X(USEPROGRAM, glUseProgram)                                                                                        \
    X(VERTEXATTRIBPOINTER, glVertexAttribPointer)                                                                      \
    X(VIEWPORT, glViewport)

#define ANYPSP_GL_DECLARE(upper, name) extern PFNGL##upper##PROC anypsp_##name;
ANYPSP_GL_FUNCTIONS(ANYPSP_GL_DECLARE)
#undef ANYPSP_GL_DECLARE

namespace anypsp::gpu {
using GlProc = void (*)();
// Resolves every function above through `resolve` (e.g. SDL_GL_GetProcAddress)
// with a context current. Returns the name of the first missing one, or null.
const char *load_gl(GlProc (*resolve)(const char *name));
} // namespace anypsp::gpu

#define glActiveTexture anypsp_glActiveTexture
#define glAttachShader anypsp_glAttachShader
#define glBindBuffer anypsp_glBindBuffer
#define glBindFramebuffer anypsp_glBindFramebuffer
#define glBindRenderbuffer anypsp_glBindRenderbuffer
#define glBindSampler anypsp_glBindSampler
#define glBindTexture anypsp_glBindTexture
#define glBindVertexArray anypsp_glBindVertexArray
#define glBlendColor anypsp_glBlendColor
#define glBlendEquation anypsp_glBlendEquation
#define glBlendFunc anypsp_glBlendFunc
#define glBlitFramebuffer anypsp_glBlitFramebuffer
#define glBufferData anypsp_glBufferData
#define glCheckFramebufferStatus anypsp_glCheckFramebufferStatus
#define glClear anypsp_glClear
#define glClearColor anypsp_glClearColor
#define glClearDepth anypsp_glClearDepth
// glClearDepthf is GL 4.1; GL 3.3 has the double form.
#define glClearDepthf(depth) anypsp_glClearDepth(static_cast<GLdouble>(depth))
#define glClearStencil anypsp_glClearStencil
#define glColorMask anypsp_glColorMask
#define glCompileShader anypsp_glCompileShader
#define glCopyTexSubImage2D anypsp_glCopyTexSubImage2D
#define glCreateProgram anypsp_glCreateProgram
#define glCreateShader anypsp_glCreateShader
#define glDeleteBuffers anypsp_glDeleteBuffers
#define glDeleteFramebuffers anypsp_glDeleteFramebuffers
#define glDeleteProgram anypsp_glDeleteProgram
#define glDeleteRenderbuffers anypsp_glDeleteRenderbuffers
#define glDeleteSamplers anypsp_glDeleteSamplers
#define glDeleteShader anypsp_glDeleteShader
#define glDeleteTextures anypsp_glDeleteTextures
#define glDeleteVertexArrays anypsp_glDeleteVertexArrays
#define glDepthFunc anypsp_glDepthFunc
#define glDepthMask anypsp_glDepthMask
#define glDisable anypsp_glDisable
#define glDrawArrays anypsp_glDrawArrays
#define glDrawBuffers anypsp_glDrawBuffers
#define glDrawElements anypsp_glDrawElements
#define glEnable anypsp_glEnable
#define glEnableVertexAttribArray anypsp_glEnableVertexAttribArray
#define glFinish anypsp_glFinish
#define glFramebufferRenderbuffer anypsp_glFramebufferRenderbuffer
#define glFramebufferTexture2D anypsp_glFramebufferTexture2D
#define glGenBuffers anypsp_glGenBuffers
#define glGenFramebuffers anypsp_glGenFramebuffers
#define glGenRenderbuffers anypsp_glGenRenderbuffers
#define glGenSamplers anypsp_glGenSamplers
#define glGenTextures anypsp_glGenTextures
#define glGenVertexArrays anypsp_glGenVertexArrays
#define glGetError anypsp_glGetError
#define glGetIntegerv anypsp_glGetIntegerv
#define glGetProgramInfoLog anypsp_glGetProgramInfoLog
#define glGetProgramiv anypsp_glGetProgramiv
#define glGetShaderInfoLog anypsp_glGetShaderInfoLog
#define glGetShaderiv anypsp_glGetShaderiv
#define glGetString anypsp_glGetString
#define glGetUniformLocation anypsp_glGetUniformLocation
#define glLinkProgram anypsp_glLinkProgram
#define glPixelStorei anypsp_glPixelStorei
#define glReadBuffer anypsp_glReadBuffer
#define glReadPixels anypsp_glReadPixels
#define glRenderbufferStorage anypsp_glRenderbufferStorage
#define glSamplerParameteri anypsp_glSamplerParameteri
#define glScissor anypsp_glScissor
#define glShaderSource anypsp_glShaderSource
#define glStencilFunc anypsp_glStencilFunc
#define glStencilMask anypsp_glStencilMask
#define glStencilOp anypsp_glStencilOp
#define glTexImage2D anypsp_glTexImage2D
#define glTexParameteri anypsp_glTexParameteri
#define glTexSubImage2D anypsp_glTexSubImage2D
#define glUniform1f anypsp_glUniform1f
#define glUniform1i anypsp_glUniform1i
#define glUniform2f anypsp_glUniform2f
#define glUniform2i anypsp_glUniform2i
#define glUniform3f anypsp_glUniform3f
#define glUniform4f anypsp_glUniform4f
#define glUniform4fv anypsp_glUniform4fv
#define glUniform4i anypsp_glUniform4i
#define glUniform4iv anypsp_glUniform4iv
#define glUseProgram anypsp_glUseProgram
#define glVertexAttribPointer anypsp_glVertexAttribPointer
#define glViewport anypsp_glViewport
