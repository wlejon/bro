#pragma once

#include <cstddef>
#include <cstdint>

// Global GL types for legacy files compiling without glad/OpenGL.
using GLuint = uint32_t;
using GLint = int32_t;
using GLenum = uint32_t;
using GLsizei = int32_t;
using GLfloat = float;
using GLboolean = uint8_t;
using GLclampd = double;
using GLsizeiptr = ptrdiff_t;
using GLuint64 = uint64_t;
using GLsync = void*;

#ifndef GL_TRIANGLES
#define GL_FALSE 0
#define GL_TRUE 1
#define GL_NONE 0
#define GL_ZERO 0
#define GL_ONE 1

#define GL_TRIANGLES 0x0004
#define GL_TRIANGLE_STRIP 0x0005
#define GL_LINES 0x0001

#define GL_LESS 0x0201
#define GL_LEQUAL 0x0203
#define GL_GREATER 0x0204
#define GL_GEQUAL 0x0206

#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303

#define GL_FRONT 0x0404
#define GL_BACK 0x0405

#define GL_CULL_FACE 0x0B44
#define GL_DEPTH_TEST 0x0B71
#define GL_STENCIL_TEST 0x0B90
#define GL_VIEWPORT 0x0BA2
#define GL_BLEND 0x0BE2
#define GL_SCISSOR_TEST 0x0C11
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_PACK_ALIGNMENT 0x0D05

#define GL_TEXTURE_2D 0x0DE1
#define GL_UNSIGNED_BYTE 0x1401
#define GL_UNSIGNED_SHORT 0x1403
#define GL_UNSIGNED_INT 0x1405
#define GL_FLOAT 0x1406
#define GL_HALF_FLOAT 0x140B

#define GL_RED 0x1903
#define GL_RGB 0x1907
#define GL_RGBA 0x1908
#define GL_DEPTH_COMPONENT 0x1902

#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_NEAREST_MIPMAP_NEAREST 0x2700
#define GL_LINEAR_MIPMAP_NEAREST 0x2701
#define GL_NEAREST_MIPMAP_LINEAR 0x2702
#define GL_LINEAR_MIPMAP_LINEAR 0x2703

#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_REPEAT 0x2901
#define GL_POLYGON_OFFSET_FILL 0x8037

#define GL_CLAMP_TO_EDGE 0x812F
#define GL_CLAMP_TO_BORDER 0x812D
#define GL_DEPTH_COMPONENT24 0x81A6
#define GL_TEXTURE_WRAP_R 0x8072
#define GL_TEXTURE_BORDER_COLOR 0x1004

#define GL_TEXTURE_CUBE_MAP 0x8513
#define GL_TEXTURE_CUBE_MAP_POSITIVE_X 0x8515
#define GL_TEXTURE_BASE_LEVEL 0x813C
#define GL_TEXTURE_MAX_LEVEL 0x813D
#define GL_TEXTURE_COMPARE_MODE 0x884C
#define GL_TEXTURE_COMPARE_FUNC 0x884D
#define GL_COMPARE_REF_TO_TEXTURE 0x884E

#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE1 0x84C1
#define GL_TEXTURE2 0x84C2
#define GL_TEXTURE3 0x84C3
#define GL_TEXTURE4 0x84C4
#define GL_TEXTURE5 0x84C5
#define GL_TEXTURE6 0x84C6
#define GL_TEXTURE7 0x84C7
#define GL_TEXTURE8 0x84C8
#define GL_TEXTURE9 0x84C9
#define GL_TEXTURE10 0x84CA
#define GL_TEXTURE11 0x84CB
#define GL_TEXTURE12 0x84CC

#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_DEPTH_BUFFER_BIT 0x00000100

#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_STATIC_DRAW 0x88E4
#define GL_DYNAMIC_DRAW 0x88E8

#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_INFO_LOG_LENGTH 0x8B84

#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#ifndef GL_DRAW_FRAMEBUFFER_BINDING
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CAB
#endif
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#define GL_RENDERBUFFER 0x8D41
#define GL_FRAMEBUFFER 0x8D40
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5

#define GL_DEPTH_COMPONENT32F 0x8CAC
#define GL_DEPTH32F_STENCIL8 0x8CAD
#define GL_DEPTH24_STENCIL8 0x88F0
#define GL_FLOAT_32_UNSIGNED_INT_24_8_REV 0x8DAD
#define GL_UNSIGNED_INT_24_8 0x84FA
#define GL_DEPTH_STENCIL 0x84F9

#define GL_RG 0x8227
#define GL_RG8 0x822B
#define GL_RG16F 0x822F
#define GL_RG32F 0x8230
#define GL_R8 0x8229
#define GL_R32F 0x822E
#define GL_RGBA8 0x8058
#define GL_RGBA16F 0x881A
#define GL_RGBA32F 0x8814
#define GL_RGB32F 0x8815
#define GL_SRGB8_ALPHA8 0x8C43

#define GL_UNIFORM_BUFFER 0x8A11
#define GL_MAX_UNIFORM_BLOCK_SIZE 0x8A30
#ifndef GL_INVALID_INDEX
#define GL_INVALID_INDEX 0xFFFFFFFFu
#endif
#define GL_INVALID_OPERATION 0x0502

#define GL_TEXTURE_2D_ARRAY 0x8C1A
#define GL_TEXTURE_3D 0x806F
#define GL_TEXTURE_BUFFER 0x8C2A
#define GL_MAX_SAMPLES 0x8D57

#define GL_TIME_ELAPSED 0x88BF
#define GL_QUERY_RESULT 0x8866
#define GL_SYNC_GPU_COMMANDS_COMPLETE 0x9117

#define GL_FLOAT_VEC2 0x8B50
#define GL_FLOAT_VEC3 0x8B51
#define GL_FLOAT_VEC4 0x8B52
#define GL_INT_VEC2 0x8B53
#define GL_INT_VEC3 0x8B54
#define GL_INT_VEC4 0x8B55
#define GL_BOOL 0x8B56
#define GL_BOOL_VEC2 0x8B57
#define GL_BOOL_VEC3 0x8B58
#define GL_BOOL_VEC4 0x8B59
#define GL_FLOAT_MAT2 0x8B5A
#define GL_FLOAT_MAT3 0x8B5B
#define GL_FLOAT_MAT4 0x8B5C
#define GL_SAMPLER_2D 0x8B5E
#define GL_SAMPLER_CUBE 0x8B60
#define GL_UNSIGNED_INT_VEC2 0x8DC6
#define GL_UNSIGNED_INT_VEC3 0x8DC7
#define GL_UNSIGNED_INT_VEC4 0x8DC8
#endif

// Dummy stubs for legacy GL functions.
#define glActiveTexture(...) ((void)0)
#define glAttachShader(...) ((void)0)
#define glBeginQuery(...) ((void)0)
#define glBindBuffer(...) ((void)0)
#define glBindBufferBase(...) ((void)0)
#define glBindFramebuffer(...) ((void)0)
#define glBindRenderbuffer(...) ((void)0)
#define glBindTexture(...) ((void)0)
#define glBindVertexArray(...) ((void)0)
#define glBlendFunc(...) ((void)0)
#define glBlendFuncSeparate(...) ((void)0)
#define glBlitFramebuffer(...) ((void)0)
#define glBufferData(...) ((void)0)
#define glBufferSubData(...) ((void)0)
#define glCheckFramebufferStatus(...) (GL_FRAMEBUFFER_COMPLETE)
#define glClear(...) ((void)0)
#define glClearColor(...) ((void)0)
#define glClearDepth(...) ((void)0)
#define glClipControl(...) ((void)0)
#define glColorMask(...) ((void)0)
#define glCompileShader(...) ((void)0)
#define glCreateProgram(...) (0)
#define glCreateShader(...) (0)
#define glCullFace(...) ((void)0)
#define glDeleteBuffers(...) ((void)0)
#define glDeleteFramebuffers(...) ((void)0)
#define glDeleteProgram(...) ((void)0)
#define glDeleteRenderbuffers(...) ((void)0)
#define glDeleteShader(...) ((void)0)
#define glDeleteTextures(...) ((void)0)
#define glDeleteVertexArrays(...) ((void)0)
#define glDepthFunc(...) ((void)0)
#define glDepthMask(...) ((void)0)
#define glDisable(...) ((void)0)
#define glDisableVertexAttribArray(...) ((void)0)
#define glDrawArrays(...) ((void)0)
#define glDrawArraysInstanced(...) ((void)0)
#define glDrawBuffer(...) ((void)0)
#define glDrawElements(...) ((void)0)
#define glDrawElementsInstanced(...) ((void)0)
#define glEnable(...) ((void)0)
#define glEnableVertexAttribArray(...) ((void)0)
#define glEndQuery(...) ((void)0)
#define glFenceSync(...) (nullptr)
#define glFlush(...) ((void)0)
#define glFramebufferRenderbuffer(...) ((void)0)
#define glFramebufferTexture2D(...) ((void)0)
#define glGenBuffers(...) ((void)0)
#define glGenerateMipmap(...) ((void)0)
#define glGenFramebuffers(...) ((void)0)
#define glGenQueries(...) ((void)0)
#define glGenRenderbuffers(...) ((void)0)
#define glGenTextures(...) ((void)0)
#define glGenVertexArrays(...) ((void)0)
#define glGetBooleanv(...) ((void)0)
#define glGetFloatv(...) ((void)0)
#define glGetIntegerv(...) ((void)0)
#define glGetProgramInfoLog(...) ((void)0)
#define glGetProgramiv(...) ((void)0)
#define glGetQueryObjectui64v(...) ((void)0)
#define glGetShaderInfoLog(...) ((void)0)
#define glGetShaderiv(...) ((void)0)
#define glGetUniformBlockIndex(...) (0)
#define glGetUniformLocation(...) (-1)
#define glLineWidth(...) ((void)0)
#define glLinkProgram(...) ((void)0)
#define glPixelStorei(...) ((void)0)
#define glPolygonOffset(...) ((void)0)
#define glReadBuffer(...) ((void)0)
#define glReadPixels(...) ((void)0)
#define glRenderbufferStorage(...) ((void)0)
#define glRenderbufferStorageMultisample(...) ((void)0)
#define glScissor(...) ((void)0)
#define glShaderSource(...) ((void)0)
#define glTexBuffer(...) ((void)0)
#define glTexImage2D(...) ((void)0)
#define glTexImage3D(...) ((void)0)
#define glTexParameterfv(...) ((void)0)
#define glTexParameteri(...) ((void)0)
#define glTexSubImage2D(...) ((void)0)
#define glTexSubImage3D(...) ((void)0)
#define glUniform1f(...) ((void)0)
#define glUniform1fv(...) ((void)0)
#define glUniform1i(...) ((void)0)
#define glUniform1iv(...) ((void)0)
#define glUniform1ui(...) ((void)0)
#define glUniform2f(...) ((void)0)
#define glUniform2fv(...) ((void)0)
#define glUniform3f(...) ((void)0)
#define glUniform3fv(...) ((void)0)
#define glUniform4f(...) ((void)0)
#define glUniform4fv(...) ((void)0)
#define glUniformBlockBinding(...) ((void)0)
#define glUniformMatrix3fv(...) ((void)0)
#define glUniformMatrix4fv(...) ((void)0)
#define glUseProgram(...) ((void)0)
#define glVertexAttribDivisor(...) ((void)0)
#define glVertexAttribIPointer(...) ((void)0)
#define glVertexAttribPointer(...) ((void)0)
#define glViewport(...) ((void)0)
