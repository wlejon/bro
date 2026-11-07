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
    X(UniformBlockBinding, uniformBlockBinding, WebGLProgram, GLuint, GLuint)                     \
    /* --- Textures and samplers --- */                                                           \
    X(CreateTexture, createTexture, GLuint)                                                       \
    X(DeleteTexture, deleteTexture, WebGLTexture)                                                 \
    X(BindTexture, bindTexture, GLenum, WebGLTexture)                                             \
    X(ActiveTexture, activeTexture, GLenum)                                                       \
    X(TexParameteri, texParameteri, GLenum, GLenum, GLint)                                        \
    X(TexParameterf, texParameterf, GLenum, GLenum, GLfloat)                                      \
    X(TexImage2DFromPBO, texImage2DFromPBO, GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, GLintptr) \
    X(TexSubImage2DFromPBO, texSubImage2DFromPBO, GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, \
      GLintptr)                                                                                   \
    X(TexImage3DFromPBO, texImage3DFromPBO, GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum, \
      GLintptr)                                                                                   \
    X(TexSubImage3DFromPBO, texSubImage3DFromPBO, GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, \
      GLenum, GLenum, GLintptr)                                                                   \
    X(TexStorage2D, texStorage2D, GLenum, GLsizei, GLenum, GLsizei, GLsizei)                      \
    X(TexStorage3D, texStorage3D, GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLsizei)             \
    X(CompressedTexImageFromPBO, compressedTexImageFromPBO, GLenum, GLint, GLenum, GLsizei, GLsizei, GLsizei, GLint, \
      GLsizei, GLintptr, bool)                                                                    \
    X(CompressedTexSubImageFromPBO, compressedTexSubImageFromPBO, GLenum, GLint, GLint, GLint, GLint, GLsizei, \
      GLsizei, GLsizei, GLenum, GLsizei, GLintptr, bool)                                          \
    X(CopyTexImage2D, copyTexImage2D, GLenum, GLint, GLenum, GLint, GLint, GLsizei, GLsizei, GLint) \
    X(CopyTexSubImage2D, copyTexSubImage2D, GLenum, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei) \
    X(CopyTexSubImage3D, copyTexSubImage3D, GLenum, GLint, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei) \
    X(GenerateMipmap, generateMipmap, GLenum)                                                     \
    X(CreateSampler, createSampler, GLuint)                                                       \
    X(DeleteSampler, deleteSampler, WebGLSampler)                                                 \
    X(BindSampler, bindSampler, GLuint, WebGLSampler)                                             \
    X(SamplerParameteri, samplerParameteri, WebGLSampler, GLenum, GLint)                          \
    X(SamplerParameterf, samplerParameterf, WebGLSampler, GLenum, GLfloat)                        \
    /* --- Framebuffers and renderbuffers --- */                                                  \
    X(CreateFramebuffer, createFramebuffer, GLuint)                                               \
    X(DeleteFramebuffer, deleteFramebuffer, WebGLFramebuffer)                                     \
    X(BindFramebuffer, bindFramebuffer, GLenum, WebGLFramebuffer)                                 \
    X(FramebufferTexture2D, framebufferTexture2D, GLenum, GLenum, GLenum, WebGLTexture, GLint)    \
    X(FramebufferTextureLayer, framebufferTextureLayer, GLenum, GLenum, WebGLTexture, GLint, GLint) \
    X(FramebufferRenderbuffer, framebufferRenderbuffer, GLenum, GLenum, GLenum, WebGLRenderbuffer) \
    X(ReadBuffer, readBuffer, GLenum)                                                             \
    X(ClearBufferfi, clearBufferfi, GLenum, GLint, GLfloat, GLint)                                \
    X(BlitFramebuffer, blitFramebuffer, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum) \
    X(ReadPixelsToPBO, readPixelsToPBO, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, GLintptr) \
    X(CreateRenderbuffer, createRenderbuffer, GLuint)                                             \
    X(DeleteRenderbuffer, deleteRenderbuffer, WebGLRenderbuffer)                                  \
    X(BindRenderbuffer, bindRenderbuffer, GLenum, WebGLRenderbuffer)                              \
    X(RenderbufferStorage, renderbufferStorage, GLenum, GLenum, GLsizei, GLsizei)                 \
    X(RenderbufferStorageMultisample, renderbufferStorageMultisample, GLenum, GLsizei, GLenum, GLsizei, GLsizei) \
    /* --- Shaders and programs (compile and link are WebGLClient's own) --- */                   \
    X(CreateShader, createShader, GLuint, GLenum)                                                 \
    X(DeleteShader, deleteShader, WebGLShader)                                                    \
    X(CreateProgram, createProgram, GLuint)                                                       \
    X(DeleteProgram, deleteProgram, WebGLProgram)                                                 \
    X(AttachShader, attachShader, WebGLProgram, WebGLShader)                                      \
    X(DetachShader, detachShader, WebGLProgram, WebGLShader)                                      \
    X(ValidateProgram, validateProgram, WebGLProgram)                                             \
    X(UseProgram, useProgram, WebGLProgram)                                                       \
    X(MaxShaderCompilerThreads, maxShaderCompilerThreads, GLuint)                                 \
    /* --- Transform feedback and syncs --- */                                                    \
    X(CreateTransformFeedback, createTransformFeedback, GLuint)                                   \
    X(DeleteTransformFeedback, deleteTransformFeedback, GLuint)                                   \
    X(BindTransformFeedback, bindTransformFeedback, GLenum, GLuint)                               \
    X(BeginTransformFeedback, beginTransformFeedback, GLenum)                                     \
    X(EndTransformFeedback, endTransformFeedback)                                                 \
    X(PauseTransformFeedback, pauseTransformFeedback)                                             \
    X(ResumeTransformFeedback, resumeTransformFeedback)                                           \
    X(DeleteSync, deleteSync, GLuint)

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
    P1(VertexAttribI4uiv, vertexAttribI4uiv, GLuint, GLuint)                                      \
    P1(DrawBuffers, drawBuffers, GLenum, GLsizei)                                                 \
    P2(ClearBufferfv, clearBufferfv, GLfloat, GLenum, GLint)                                      \
    P2(ClearBufferiv, clearBufferiv, GLint, GLenum, GLint)                                        \
    P2(ClearBufferuiv, clearBufferuiv, GLuint, GLenum, GLint)

// X(Name, backendMethod, ArgTypes...): the arguments, then client bytes as
// (const void* data, size_t size), the backend's own last two parameters. A
// null pointer is "no data" and replays as (nullptr, 0); otherwise the bytes
// are copied like any payload, and replay as (copy, size).
#define BRO_WEBGL_BYTES_COMMANDS(X)                                                               \
    X(TexImage2D, texImage2D, GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum)      \
    X(TexSubImage2D, texSubImage2D, GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum) \
    X(TexImage3D, texImage3D, GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum) \
    X(TexSubImage3D, texSubImage3D, GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLenum, GLenum) \
    X(CompressedTexImage2D, compressedTexImage2D, GLenum, GLint, GLenum, GLsizei, GLsizei, GLint) \
    X(CompressedTexSubImage2D, compressedTexSubImage2D, GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum) \
    X(CompressedTexImage3D, compressedTexImage3D, GLenum, GLint, GLenum, GLsizei, GLsizei, GLsizei, GLint) \
    X(CompressedTexSubImage3D, compressedTexSubImage3D, GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, \
      GLsizei, GLenum)

// X(Name, backendMethod, ArgTypes...): the arguments, then a string the
// backend takes as `const std::string&`, copied like a payload.
#define BRO_WEBGL_STRING_COMMANDS(X)                                                              \
    X(ShaderSource, shaderSource, WebGLShader)                                                    \
    X(BindAttribLocation, bindAttribLocation, WebGLProgram, GLuint)                               \
    X(EnableExtension, enableExtension)

// Hand-written records (webgl_client.h encodes, webgl_replay.cpp decodes):
//   BufferData     {target, usage, size} + payload (Null: allocate only)
//   BufferSubData  {target, offset} + payload
//   (and the records of webgl_client_records.h: the DOM-source uploads, the
//   lists, and the calls whose results the backend publishes to the client)
#define BRO_WEBGL_CUSTOM_COMMANDS(X) \
    X(BufferData)                    \
    X(BufferSubData)                 \
    X(TexImage2DSource)              \
    X(TexSubImage2DSource)           \
    X(InvalidateFramebuffer)         \
    X(InvalidateSubFramebuffer)      \
    X(TransformFeedbackVaryings)     \
    X(CompileShader)                 \
    X(LinkProgram)                   \
    X(CreateQuery)                   \
    X(DeleteQuery)                   \
    X(BeginQuery)                    \
    X(EndQuery)                      \
    X(QueryCounter)                  \
    X(FenceSync)

// The channel's own records (webgl_channel.cpp replays them; the backend
// never sees them): the backend made, dropped and destroyed on the WebGL
// thread, and a round trip's callback.
//   Attach    {width, height}: a new backend (the context's first, or restored)
//   Detach    {}: the backend goes (context loss)
//   SyncCall  {fn, ctx, sequence}: fn(ctx, backend), then the answer acknowledged
//   Destroy   {}: the backend goes and the channel with it; nothing follows
#define BRO_WEBGL_CHANNEL_COMMANDS(X) \
    X(Attach)                         \
    X(Detach)                         \
    X(SyncCall)                       \
    X(Destroy)
