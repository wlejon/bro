#pragma once

// The WebGL command table: one line per command the client encodes into its
// ring and the replay decodes onto vk::WebGLVkContext. A line names the
// command (its opcode), the backend method it replays as, and the argument
// types in the method's order; the record carries the arguments packed in
// that order (webgl_cmd.h). Encoding (WebGLClient's generated methods) and
// replay (webgl_replay.cpp) both come out of these lists, so the two can not
// disagree about a record's layout.
//
// A backend method's return value (bindBuffer's refusal, createBuffer's
// object) is dropped: what a call answers is the client's, and what it
// refused is the backend's pending error, which getError reads in order.

// X(Name, backendMethod, ArgTypes...): fire-and-forget, no payload.
#define BRO_WEBGL_SIMPLE_COMMANDS(X)                                                              \
    /* --- State --- */                                                                           \
    X(Viewport, viewport, GLint, GLint, GLsizei, GLsizei)                                         \
    X(Scissor, scissor, GLint, GLint, GLsizei, GLsizei)                                           \
    X(ClearColor, clearColor, GLfloat, GLfloat, GLfloat, GLfloat)                                 \
    X(ClearDepth, clearDepth, GLfloat)                                                            \
    X(ClearStencil, clearStencil, GLint)                                                          \
    X(Clear, clear, GLbitfield)                                                                   \
    X(Enable, enable, GLenum)                                                                     \
    X(Disable, disable, GLenum)                                                                   \
    X(DepthFunc, depthFunc, GLenum)                                                               \
    X(DepthMask, depthMask, GLboolean)                                                            \
    X(DepthRange, depthRange, GLfloat, GLfloat)                                                   \
    X(BlendFunc, blendFunc, GLenum, GLenum)                                                       \
    X(BlendFuncSeparate, blendFuncSeparate, GLenum, GLenum, GLenum, GLenum)                       \
    X(BlendEquation, blendEquation, GLenum)                                                       \
    X(BlendEquationSeparate, blendEquationSeparate, GLenum, GLenum)                               \
    X(BlendColor, blendColor, GLfloat, GLfloat, GLfloat, GLfloat)                                 \
    X(ColorMask, colorMask, GLboolean, GLboolean, GLboolean, GLboolean)                           \
    X(StencilFunc, stencilFunc, GLenum, GLint, GLuint)                                            \
    X(StencilFuncSeparate, stencilFuncSeparate, GLenum, GLenum, GLint, GLuint)                    \
    X(StencilOp, stencilOp, GLenum, GLenum, GLenum)                                               \
    X(StencilOpSeparate, stencilOpSeparate, GLenum, GLenum, GLenum, GLenum)                       \
    X(StencilMask, stencilMask, GLuint)                                                           \
    X(StencilMaskSeparate, stencilMaskSeparate, GLenum, GLuint)                                   \
    X(CullFace, cullFace, GLenum)                                                                 \
    X(FrontFace, frontFace, GLenum)                                                               \
    X(LineWidth, lineWidth, GLfloat)                                                              \
    X(PolygonOffset, polygonOffset, GLfloat, GLfloat)                                             \
    X(SampleCoverage, sampleCoverage, GLfloat, GLboolean)                                         \
    X(Hint, hint, GLenum, GLenum)                                                                 \
    X(PixelStorei, pixelStorei, GLenum, GLint)                                                    \
    /* --- Draws --- */                                                                           \
    X(DrawArrays, drawArrays, GLenum, GLint, GLsizei)                                             \
    X(DrawElements, drawElements, GLenum, GLsizei, GLenum, uintptr_t)                             \
    X(DrawArraysInstanced, drawArraysInstanced, GLenum, GLint, GLsizei, GLsizei)                  \
    X(DrawElementsInstanced, drawElementsInstanced, GLenum, GLsizei, GLenum, uintptr_t, GLsizei)  \
    X(DrawRangeElements, drawRangeElements, GLenum, GLuint, GLuint, GLsizei, GLenum, uintptr_t)   \
    /* --- Buffers and vertex arrays --- */                                                       \
    X(CreateBuffer, createBuffer, GLuint)                                                         \
    X(DeleteBuffer, deleteBuffer, WebGLBuffer)                                                    \
    X(BindBuffer, bindBuffer, GLenum, WebGLBuffer)                                                \
    X(BindBufferBase, bindBufferBase, GLenum, GLuint, WebGLBuffer)                                \
    X(BindBufferRange, bindBufferRange, GLenum, GLuint, WebGLBuffer, GLintptr, GLsizeiptr)        \
    X(CopyBufferSubData, copyBufferSubData, GLenum, GLenum, GLintptr, GLintptr, GLsizeiptr)       \
    X(CreateVertexArray, createVertexArray, GLuint)                                               \
    X(DeleteVertexArray, deleteVertexArray, WebGLVertexArrayObject)                               \
    X(BindVertexArray, bindVertexArray, WebGLVertexArrayObject)                                   \
    X(VertexAttribPointer, vertexAttribPointer, GLuint, GLint, GLenum, GLboolean, GLsizei, uintptr_t) \
    X(VertexAttribIPointer, vertexAttribIPointer, GLuint, GLint, GLenum, GLsizei, uintptr_t)      \
    X(EnableVertexAttribArray, enableVertexAttribArray, GLuint)                                   \
    X(DisableVertexAttribArray, disableVertexAttribArray, GLuint)                                 \
    X(VertexAttribDivisor, vertexAttribDivisor, GLuint, GLuint)                                   \
    X(VertexAttribI4i, vertexAttribI4i, GLuint, GLint, GLint, GLint, GLint)                       \
    X(VertexAttribI4ui, vertexAttribI4ui, GLuint, GLuint, GLuint, GLuint, GLuint)                 \
    X(VertexAttrib1f, vertexAttrib1f, GLuint, GLfloat)                                            \
    X(VertexAttrib2f, vertexAttrib2f, GLuint, GLfloat, GLfloat)                                   \
    X(VertexAttrib3f, vertexAttrib3f, GLuint, GLfloat, GLfloat, GLfloat)                          \
    X(VertexAttrib4f, vertexAttrib4f, GLuint, GLfloat, GLfloat, GLfloat, GLfloat)                 \
    /* --- Scalar uniforms --- */                                                                 \
    X(Uniform1f, uniform1f, WebGLUniformLocation, GLfloat)                                        \
    X(Uniform2f, uniform2f, WebGLUniformLocation, GLfloat, GLfloat)                               \
    X(Uniform3f, uniform3f, WebGLUniformLocation, GLfloat, GLfloat, GLfloat)                      \
    X(Uniform4f, uniform4f, WebGLUniformLocation, GLfloat, GLfloat, GLfloat, GLfloat)             \
    X(Uniform1i, uniform1i, WebGLUniformLocation, GLint)                                          \
    X(Uniform2i, uniform2i, WebGLUniformLocation, GLint, GLint)                                   \
    X(Uniform3i, uniform3i, WebGLUniformLocation, GLint, GLint, GLint)                            \
    X(Uniform4i, uniform4i, WebGLUniformLocation, GLint, GLint, GLint, GLint)                     \
    X(Uniform1ui, uniform1ui, WebGLUniformLocation, GLuint)                                       \
    X(Uniform2ui, uniform2ui, WebGLUniformLocation, GLuint, GLuint)                               \
    X(Uniform3ui, uniform3ui, WebGLUniformLocation, GLuint, GLuint, GLuint)                       \
    X(Uniform4ui, uniform4ui, WebGLUniformLocation, GLuint, GLuint, GLuint, GLuint)               \
    X(UniformBlockBinding, uniformBlockBinding, WebGLProgram, GLuint, GLuint)

// X(Name, backendMethod, ArgTypes...): the client's own records, encoded by
// hand-written WebGLClient members (no generated method of the same name).
#define BRO_WEBGL_CONTROL_COMMANDS(X)                                                             \
    X(SyntheticError, setSyntheticError, GLenum)                                                  \
    X(Flush, flush)                                                                               \
    X(Presented, presented)                                                                       \
    X(Resize, resize, int, int, uint64_t)

// P<n>(Name, backendMethod, Elem, ArgTypes...): n arguments, then a pointer
// to Elem the call reads. The client copies the elements into the record (a
// blob past kInlinePayloadMax); the replay hands the method that copy.
#define BRO_WEBGL_PAYLOAD_COMMANDS(P1, P2, P3)                                                    \
    P2(Uniform1fv, uniform1fv, GLfloat, WebGLUniformLocation, GLsizei)                            \
    P2(Uniform2fv, uniform2fv, GLfloat, WebGLUniformLocation, GLsizei)                            \
    P2(Uniform3fv, uniform3fv, GLfloat, WebGLUniformLocation, GLsizei)                            \
    P2(Uniform4fv, uniform4fv, GLfloat, WebGLUniformLocation, GLsizei)                            \
    P2(Uniform1iv, uniform1iv, GLint, WebGLUniformLocation, GLsizei)                              \
    P2(Uniform2iv, uniform2iv, GLint, WebGLUniformLocation, GLsizei)                              \
    P2(Uniform3iv, uniform3iv, GLint, WebGLUniformLocation, GLsizei)                              \
    P2(Uniform4iv, uniform4iv, GLint, WebGLUniformLocation, GLsizei)                              \
    P2(Uniform1uiv, uniform1uiv, GLuint, WebGLUniformLocation, GLsizei)                           \
    P2(Uniform2uiv, uniform2uiv, GLuint, WebGLUniformLocation, GLsizei)                           \
    P2(Uniform3uiv, uniform3uiv, GLuint, WebGLUniformLocation, GLsizei)                           \
    P2(Uniform4uiv, uniform4uiv, GLuint, WebGLUniformLocation, GLsizei)                           \
    P3(UniformMatrix2fv, uniformMatrix2fv, GLfloat, WebGLUniformLocation, GLsizei, GLboolean)     \
    P3(UniformMatrix3fv, uniformMatrix3fv, GLfloat, WebGLUniformLocation, GLsizei, GLboolean)     \
    P3(UniformMatrix4fv, uniformMatrix4fv, GLfloat, WebGLUniformLocation, GLsizei, GLboolean)     \
    P3(UniformMatrix2x3fv, uniformMatrix2x3fv, GLfloat, WebGLUniformLocation, GLsizei, GLboolean) \
    P3(UniformMatrix3x2fv, uniformMatrix3x2fv, GLfloat, WebGLUniformLocation, GLsizei, GLboolean) \
    P3(UniformMatrix2x4fv, uniformMatrix2x4fv, GLfloat, WebGLUniformLocation, GLsizei, GLboolean) \
    P3(UniformMatrix4x2fv, uniformMatrix4x2fv, GLfloat, WebGLUniformLocation, GLsizei, GLboolean) \
    P3(UniformMatrix3x4fv, uniformMatrix3x4fv, GLfloat, WebGLUniformLocation, GLsizei, GLboolean) \
    P3(UniformMatrix4x3fv, uniformMatrix4x3fv, GLfloat, WebGLUniformLocation, GLsizei, GLboolean) \
    P1(VertexAttrib1fv, vertexAttrib1fv, GLfloat, GLuint)                                         \
    P1(VertexAttrib2fv, vertexAttrib2fv, GLfloat, GLuint)                                         \
    P1(VertexAttrib3fv, vertexAttrib3fv, GLfloat, GLuint)                                         \
    P1(VertexAttrib4fv, vertexAttrib4fv, GLfloat, GLuint)                                         \
    P1(VertexAttribI4iv, vertexAttribI4iv, GLint, GLuint)                                         \
    P1(VertexAttribI4uiv, vertexAttribI4uiv, GLuint, GLuint)

// Hand-written records (webgl_client.h encodes, webgl_replay.cpp decodes):
//   BufferData     {target, usage, size} + payload (Null: allocate only)
//   BufferSubData  {target, offset} + payload
#define BRO_WEBGL_CUSTOM_COMMANDS(X) \
    X(BufferData)                    \
    X(BufferSubData)
