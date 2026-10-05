#pragma once

#include "render/vulkan_context.h"
#include "webgl/webgl_objects.h"
#include "webgl/vulkan/webgl_vk_types.h"
#include "webgl/vulkan/webgl_vk_canvas.h"
#include "webgl/vulkan/webgl_vk_pipeline.h"
#include "webgl/vulkan/webgl_vk_stream.h"
#include "webgl/vulkan/webgl_vk_pixels.h"

#include <vulkan/vulkan.h>
#include "webgl/webgl_types.h"
#include <map>
#include <memory>
#include <span>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace bro::webgl::vk {

/// The WebGL2 API on Vulkan: every WebGL2RenderingContext call the JS
/// binding makes lands here, against this context's own state, objects and
/// drawing buffer (the canvas). Errors are recorded GL-style, the first one
/// pending until getError.
class WebGLVkContext {
public:
    WebGLVkContext(int width, int height, render::VulkanContext& context, GLuint firstObjectId = 1);
    ~WebGLVkContext();

    /// The name the next created object gets.
    GLuint nextObjectId() const { return nextObjectId_; }

    WebGLVkContext(const WebGLVkContext&) = delete;
    WebGLVkContext& operator=(const WebGLVkContext&) = delete;

    /// Resize canvas attachments.
    void resize(int width, int height);

    /// Offscreen canvas manager.
    WebGLVkCanvas& canvas() { return canvas_; }
    const WebGLVkCanvas& canvas() const { return canvas_; }

    int canvasWidth() const { return static_cast<int>(canvas_.width()); }
    int canvasHeight() const { return static_cast<int>(canvas_.height()); }

    /// Read canvas color pixels as top-down RGBA bytes.
    bool readCanvasPixels(std::vector<uint8_t>& out);

    // =================================================================
    // WebGL2 API Commands
    // =================================================================

    // --- State ---
    void viewport(GLint x, GLint y, GLsizei w, GLsizei h);
    void scissor(GLint x, GLint y, GLsizei w, GLsizei h);
    void clearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
    void clearDepth(GLfloat depth);
    void clearStencil(GLint s);
    void clear(GLbitfield mask);
    void clearBufferfv(GLenum buffer, GLint drawbuffer, const GLfloat* values);
    void clearBufferiv(GLenum buffer, GLint drawbuffer, const GLint* values);
    void clearBufferuiv(GLenum buffer, GLint drawbuffer, const GLuint* values);
    void clearBufferfi(GLenum buffer, GLint drawbuffer, GLfloat depth, GLint stencil);
    void enable(GLenum cap);
    void disable(GLenum cap);
    GLboolean isEnabled(GLenum cap);
    void depthFunc(GLenum func);
    void depthMask(GLboolean flag);
    void depthRange(GLfloat zNear, GLfloat zFar);
    void blendFunc(GLenum sfactor, GLenum dfactor);
    void blendFuncSeparate(GLenum srcRGB, GLenum dstRGB, GLenum srcAlpha, GLenum dstAlpha);
    void blendEquation(GLenum mode);
    void blendEquationSeparate(GLenum modeRGB, GLenum modeAlpha);
    void blendColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
    void colorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a);
    void stencilFunc(GLenum func, GLint ref, GLuint mask);
    void stencilFuncSeparate(GLenum face, GLenum func, GLint ref, GLuint mask);
    void stencilOp(GLenum fail, GLenum zfail, GLenum zpass);
    void stencilOpSeparate(GLenum face, GLenum fail, GLenum zfail, GLenum zpass);
    void stencilMask(GLuint mask);
    void stencilMaskSeparate(GLenum face, GLuint mask);
    void cullFace(GLenum mode);
    void frontFace(GLenum mode);
    void lineWidth(GLfloat width);
    void polygonOffset(GLfloat factor, GLfloat units);
    void sampleCoverage(GLfloat value, GLboolean invert);
    void hint(GLenum target, GLenum mode);
    GLenum getError();
    void setSyntheticError(GLenum err);
    void pixelStorei(GLenum pname, GLint param);
    GLint getPixelStorei(GLenum pname) const;

    GLint getParameterInt(GLenum pname);
    int64_t getParameterInt64(GLenum pname);  // the 64-bit limits (MAX_ELEMENT_INDEX, ...)
    GLfloat getParameterFloat(GLenum pname);
    GLboolean getParameterBool(GLenum pname);
    void getParameterInt2(GLenum pname, GLint* out);
    void getParameterInt4(GLenum pname, GLint* out);
    void getParameterFloat2(GLenum pname, GLfloat* out);
    void getParameterFloat4(GLenum pname, GLfloat* out);
    void getParameterBool4(GLenum pname, GLboolean* out);

    /// getSupportedExtensions: what this device backs. enableExtension is
    /// getExtension's: true when supported, and a compressed-texture
    /// extension's formats are accepted from then on.
    std::vector<std::string> supportedExtensions() const;
    bool enableExtension(const std::string& name);

    // --- Buffers ---
    WebGLBuffer createBuffer();
    void deleteBuffer(WebGLBuffer buf);
    GLboolean isBuffer(WebGLBuffer buf) const;  // true once bound
    bool bindBuffer(GLenum target, WebGLBuffer buf);  // false (with the error) when refused
    void bindBufferBase(GLenum target, GLuint index, WebGLBuffer buf);
    void bindBufferRange(GLenum target, GLuint index, WebGLBuffer buf, GLintptr offset, GLsizeiptr size);
    /// UNIFORM_BUFFER_START / UNIFORM_BUFFER_SIZE of an indexed binding.
    int64_t getIndexedBufferParameter(GLenum pname, GLuint index);
    void bufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage);
    void bufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void* data);
    void getBufferSubData(GLenum target, GLintptr srcByteOffset, void* dstData, GLsizeiptr length);
    void copyBufferSubData(GLenum readTarget, GLenum writeTarget, GLintptr readOffset, GLintptr writeOffset, GLsizeiptr size);
    void* mapBufferRange(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access);
    bool unmapBuffer(GLenum target);
    void flushMappedBufferRange(GLenum target, GLintptr offset, GLsizeiptr length);
    GLuint boundBuffer(GLenum target);
    int64_t boundBufferSize(GLenum target);
    /// BUFFER_SIZE / BUFFER_USAGE of the buffer bound to `target`.
    bool getBufferParameter(GLenum target, GLenum pname, GLint& out);

    void readPixelsToPBO(GLint x, GLint y, GLsizei width, GLsizei height,
                         GLenum format, GLenum type, GLintptr offset);

    // --- Shaders & Programs ---
    WebGLShader createShader(GLenum type);
    void deleteShader(WebGLShader s);
    void shaderSource(WebGLShader s, const std::string& src);
    void compileShader(WebGLShader s);
    GLint getShaderParameter(WebGLShader s, GLenum pname);
    std::string getShaderInfoLog(WebGLShader s);
    std::string getShaderSource(WebGLShader s);
    GLboolean isShader(WebGLShader s) const;

    WebGLProgram createProgram();
    void deleteProgram(WebGLProgram p);
    GLboolean isProgram(WebGLProgram p) const;
    GLuint currentProgram() const { return currentProgramId_; }
    std::vector<GLuint> attachedShaders(WebGLProgram p) const;
    void attachShader(WebGLProgram p, WebGLShader s);
    void detachShader(WebGLProgram p, WebGLShader s);
    void linkProgram(WebGLProgram p);
    void useProgram(WebGLProgram p);
    GLint getProgramParameter(WebGLProgram p, GLenum pname);
    /// VALIDATE_STATUS: linked, and no texture unit read by samplers of two types.
    void validateProgram(WebGLProgram p);
    /// The value at `loc` of `p`'s default block, or a sampler's unit.
    bool getUniform(WebGLProgram p, WebGLUniformLocation loc, GLValue& out);
    std::string getProgramInfoLog(WebGLProgram p);
    GLint getFragDataLocation(WebGLProgram p, const std::string& name);
    void bindAttribLocation(WebGLProgram p, GLuint index, const std::string& name);
    GLuint getUniformBlockIndex(WebGLProgram p, const std::string& name);
    void uniformBlockBinding(WebGLProgram p, GLuint blockIndex, GLuint bindingPoint);
    std::vector<GLuint> getUniformIndices(WebGLProgram p, const std::vector<std::string>& names);
    std::vector<GLint> getActiveUniforms(WebGLProgram p, const std::vector<GLuint>& indices, GLenum pname);
    GLint getActiveUniformBlockParameteri(WebGLProgram p, GLuint blockIndex, GLenum pname);
    std::vector<GLint> getActiveUniformBlockIndices(WebGLProgram p, GLuint blockIndex);
    std::string getActiveUniformBlockName(WebGLProgram p, GLuint blockIndex);
    WebGLActiveInfo getActiveAttrib(WebGLProgram p, GLuint index);
    WebGLActiveInfo getActiveUniform(WebGLProgram p, GLuint index);

    // --- Uniforms & Attributes ---
    GLint getAttribLocation(WebGLProgram p, const std::string& name);
    WebGLUniformLocation getUniformLocation(WebGLProgram p, const std::string& name);
    void uniform1f(WebGLUniformLocation loc, GLfloat v0);
    void uniform2f(WebGLUniformLocation loc, GLfloat v0, GLfloat v1);
    void uniform3f(WebGLUniformLocation loc, GLfloat v0, GLfloat v1, GLfloat v2);
    void uniform4f(WebGLUniformLocation loc, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3);
    void uniform1i(WebGLUniformLocation loc, GLint v0);
    void uniform2i(WebGLUniformLocation loc, GLint v0, GLint v1);
    void uniform3i(WebGLUniformLocation loc, GLint v0, GLint v1, GLint v2);
    void uniform4i(WebGLUniformLocation loc, GLint v0, GLint v1, GLint v2, GLint v3);
    void uniform1ui(WebGLUniformLocation loc, GLuint v0);
    void uniform2ui(WebGLUniformLocation loc, GLuint v0, GLuint v1);
    void uniform3ui(WebGLUniformLocation loc, GLuint v0, GLuint v1, GLuint v2);
    void uniform4ui(WebGLUniformLocation loc, GLuint v0, GLuint v1, GLuint v2, GLuint v3);
    void uniform1fv(WebGLUniformLocation loc, GLsizei count, const GLfloat* v);
    void uniform2fv(WebGLUniformLocation loc, GLsizei count, const GLfloat* v);
    void uniform3fv(WebGLUniformLocation loc, GLsizei count, const GLfloat* v);
    void uniform4fv(WebGLUniformLocation loc, GLsizei count, const GLfloat* v);
    void uniform1iv(WebGLUniformLocation loc, GLsizei count, const GLint* v);
    void uniform2iv(WebGLUniformLocation loc, GLsizei count, const GLint* v);
    void uniform3iv(WebGLUniformLocation loc, GLsizei count, const GLint* v);
    void uniform4iv(WebGLUniformLocation loc, GLsizei count, const GLint* v);
    void uniform1uiv(WebGLUniformLocation loc, GLsizei count, const GLuint* v);
    void uniform2uiv(WebGLUniformLocation loc, GLsizei count, const GLuint* v);
    void uniform3uiv(WebGLUniformLocation loc, GLsizei count, const GLuint* v);
    void uniform4uiv(WebGLUniformLocation loc, GLsizei count, const GLuint* v);
    void uniformMatrix2fv(WebGLUniformLocation loc, GLsizei count, GLboolean transpose, const GLfloat* value);
    void uniformMatrix3fv(WebGLUniformLocation loc, GLsizei count, GLboolean transpose, const GLfloat* value);
    void uniformMatrix4fv(WebGLUniformLocation loc, GLsizei count, GLboolean transpose, const GLfloat* value);
    void uniformMatrix2x3fv(WebGLUniformLocation loc, GLsizei count, GLboolean transpose, const GLfloat* value);
    void uniformMatrix3x2fv(WebGLUniformLocation loc, GLsizei count, GLboolean transpose, const GLfloat* value);
    void uniformMatrix2x4fv(WebGLUniformLocation loc, GLsizei count, GLboolean transpose, const GLfloat* value);
    void uniformMatrix4x2fv(WebGLUniformLocation loc, GLsizei count, GLboolean transpose, const GLfloat* value);
    void uniformMatrix3x4fv(WebGLUniformLocation loc, GLsizei count, GLboolean transpose, const GLfloat* value);
    void uniformMatrix4x3fv(WebGLUniformLocation loc, GLsizei count, GLboolean transpose, const GLfloat* value);

    // --- Vertex Arrays (VAO) ---
    WebGLVertexArrayObject createVertexArray();
    void deleteVertexArray(WebGLVertexArrayObject vao);
    bool bindVertexArray(WebGLVertexArrayObject vao);
    GLboolean isVertexArray(WebGLVertexArrayObject vao) const;  // true once bound
    GLuint currentVertexArray() const { return currentVaoId_; }
    void enableVertexAttribArray(GLuint index);
    void disableVertexAttribArray(GLuint index);
    void vertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized,
                             GLsizei stride, uintptr_t offset);
    void vertexAttribIPointer(GLuint index, GLint size, GLenum type,
                              GLsizei stride, uintptr_t offset);
    void vertexAttribDivisor(GLuint index, GLuint divisor);
    /// The current vertex array's attribute `index`, or its generic value.
    bool getVertexAttrib(GLuint index, GLenum pname, GLValue& out);
    GLintptr getVertexAttribOffset(GLuint index, GLenum pname);

    void vertexAttrib1f(GLuint index, GLfloat x);
    void vertexAttrib2f(GLuint index, GLfloat x, GLfloat y);
    void vertexAttrib3f(GLuint index, GLfloat x, GLfloat y, GLfloat z);
    void vertexAttrib4f(GLuint index, GLfloat x, GLfloat y, GLfloat z, GLfloat w);
    void vertexAttrib1fv(GLuint index, const GLfloat* v);
    void vertexAttrib2fv(GLuint index, const GLfloat* v);
    void vertexAttrib3fv(GLuint index, const GLfloat* v);
    void vertexAttrib4fv(GLuint index, const GLfloat* v);
    void vertexAttribI4i(GLuint index, GLint x, GLint y, GLint z, GLint w);
    void vertexAttribI4ui(GLuint index, GLuint x, GLuint y, GLuint z, GLuint w);
    void vertexAttribI4iv(GLuint index, const GLint* v);
    void vertexAttribI4uiv(GLuint index, const GLuint* v);

    // --- Drawing ---
    void drawArrays(GLenum mode, GLint first, GLsizei count);
    void drawElements(GLenum mode, GLsizei count, GLenum type, uintptr_t offset);
    void drawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instanceCount);
    void drawElementsInstanced(GLenum mode, GLsizei count, GLenum type, uintptr_t offset, GLsizei instanceCount);
    void drawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, uintptr_t offset);

    // --- Textures ---
    // Uploads take the client bytes and how many there are: fewer than the
    // unpack state reads is INVALID_OPERATION. A null pointer defines the
    // image as zeros. With a PIXEL_UNPACK_BUFFER bound, the *FromPBO forms
    // read at a byte offset into it instead.
    WebGLTexture createTexture();
    void deleteTexture(WebGLTexture tex);
    void bindTexture(GLenum target, WebGLTexture tex);
    void activeTexture(GLenum texture);
    GLboolean isTexture(WebGLTexture tex) const;
    GLuint boundTexture(GLenum target) const;  // TEXTURE_BINDING_* of the active unit
    GLuint activeTextureUnit() const { return activeTextureUnit_; }
    void texParameteri(GLenum target, GLenum pname, GLint param);
    void texParameterf(GLenum target, GLenum pname, GLfloat param);
    /// getTexParameter: false (with the GL error) for an invalid query.
    using TexParameter = TexParameterValue;
    bool getTexParameter(GLenum target, GLenum pname, TexParameter& out);
    void texImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border,
                    GLenum format, GLenum type, const void* pixels, size_t size);
    void texSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height,
                       GLenum format, GLenum type, const void* pixels, size_t size);
    void texImage3D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
                    GLsizei depth, GLint border, GLenum format, GLenum type, const void* pixels, size_t size);
    void texSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width,
                       GLsizei height, GLsizei depth, GLenum format, GLenum type, const void* pixels, size_t size);
    /// An image decoded from a DOM source (img, canvas, video, ImageBitmap,
    /// ImageData): tightly packed RGBA8 rows, top row first, as client data
    /// of RGBA / UNSIGNED_BYTE converted to `format` / `type`. width/height
    /// < 0 take the source's own size.
    void texImage2DSource(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
                          GLenum format, GLenum type, const uint8_t* rgba, uint32_t srcWidth, uint32_t srcHeight);
    void texSubImage2DSource(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width,
                             GLsizei height, GLenum format, GLenum type, const uint8_t* rgba, uint32_t srcWidth,
                             uint32_t srcHeight);
    void texImage2DFromPBO(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
                           GLint border, GLenum format, GLenum type, GLintptr offset);
    void texSubImage2DFromPBO(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width,
                              GLsizei height, GLenum format, GLenum type, GLintptr offset);
    void texImage3DFromPBO(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
                           GLsizei depth, GLint border, GLenum format, GLenum type, GLintptr offset);
    void texSubImage3DFromPBO(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset,
                              GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type,
                              GLintptr offset);
    void texStorage2D(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height);
    void texStorage3D(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height,
                      GLsizei depth);
    // Compressed images are stored as they come: only the formats of the
    // extensions this device backs (compressedTextureFormats) are accepted.
    // A PBO source is `size` bytes at byte `offset` of the bound buffer.
    std::vector<GLint> compressedTextureFormats() const;
    void compressedTexImage2D(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height,
                              GLint border, const void* data, size_t size);
    void compressedTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width,
                                 GLsizei height, GLenum format, const void* data, size_t size);
    void compressedTexImage3D(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height,
                              GLsizei depth, GLint border, const void* data, size_t size);
    void compressedTexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset,
                                 GLsizei width, GLsizei height, GLsizei depth, GLenum format, const void* data,
                                 size_t size);
    void compressedTexImageFromPBO(GLenum target, GLint level, GLenum internalformat, GLsizei width,
                                   GLsizei height, GLsizei depth, GLint border, GLsizei size, GLintptr offset,
                                   bool is3D);
    void compressedTexSubImageFromPBO(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset,
                                      GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLsizei size,
                                      GLintptr offset, bool is3D);
    void copyTexImage2D(GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width,
                        GLsizei height, GLint border);
    void copyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y,
                           GLsizei width, GLsizei height);
    void copyTexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLint x,
                           GLint y, GLsizei width, GLsizei height);
    void generateMipmap(GLenum target);

    // --- Samplers ---
    WebGLSampler createSampler();
    void deleteSampler(WebGLSampler s);
    void bindSampler(GLuint unit, WebGLSampler s);
    void samplerParameteri(WebGLSampler s, GLenum pname, GLint param);
    void samplerParameterf(WebGLSampler s, GLenum pname, GLfloat param);
    GLint getSamplerParameteri(WebGLSampler s, GLenum pname);
    GLfloat getSamplerParameterf(WebGLSampler s, GLenum pname);
    GLboolean isSampler(WebGLSampler s);
    GLuint boundSampler(GLuint unit) const { return unit < boundSamplers_.size() ? boundSamplers_[unit] : 0; }

    // --- Framebuffers & Renderbuffers ---
    WebGLFramebuffer createFramebuffer();
    void deleteFramebuffer(WebGLFramebuffer fb);
    void bindFramebuffer(GLenum target, WebGLFramebuffer fb);
    GLboolean isFramebuffer(WebGLFramebuffer fb) const;  // true once bound
    void framebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget,
                              WebGLTexture tex, GLint level);
    void framebufferRenderbuffer(GLenum target, GLenum attachment, GLenum renderbuffertarget,
                                 WebGLRenderbuffer rbo);
    void framebufferTextureLayer(GLenum target, GLenum attachment, WebGLTexture tex, GLint level, GLint layer);
    GLenum checkFramebufferStatus(GLenum target);
    void drawBuffers(GLsizei n, const GLenum* bufs);
    void readBuffer(GLenum src);
    /// Validated, and otherwise a hint the attachments' contents satisfy.
    void invalidateFramebuffer(GLenum target, std::span<const GLenum> attachments);
    void invalidateSubFramebuffer(GLenum target, std::span<const GLenum> attachments, GLint x, GLint y,
                                  GLsizei width, GLsizei height);
    void blitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1,
                         GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1,
                         GLbitfield mask, GLenum filter);

    GLuint drawFramebufferBinding() const { return drawFboId_; }
    GLuint readFramebufferBinding() const { return readFboId_; }
    GLuint renderbufferBinding() const { return currentRenderbufferId_; }
    GLenum drawBufferState(GLuint i) const;   // getParameter(DRAW_BUFFERi)
    GLenum readBufferState() const;           // getParameter(READ_BUFFER)

    /// getFramebufferAttachmentParameter: `objectType`/`objectName` are set
    /// for FRAMEBUFFER_ATTACHMENT_OBJECT_NAME (the caller wraps the object),
    /// `value` for everything else. False (with the GL error) when invalid;
    /// `isNull` when WebGL answers null.
    struct AttachmentParameter {
        GLint value = 0;
        GLenum objectType = GL_NONE;
        GLuint objectName = 0;
        bool isNull = false;
    };
    bool getFramebufferAttachmentParameter(GLenum target, GLenum attachment, GLenum pname,
                                           AttachmentParameter& out);
    GLint getRenderbufferParameter(GLenum target, GLenum pname);
    /// The sample counts renderbufferStorageMultisample accepts for
    /// `internalformat`, highest first (getInternalformatParameter(SAMPLES)).
    std::vector<GLint> supportedSampleCounts(GLenum internalformat);

    WebGLRenderbuffer createRenderbuffer();
    void deleteRenderbuffer(WebGLRenderbuffer rbo);
    void bindRenderbuffer(GLenum target, WebGLRenderbuffer rbo);
    GLboolean isRenderbuffer(WebGLRenderbuffer rbo) const;  // true once bound
    void renderbufferStorage(GLenum target, GLenum internalformat, GLsizei width, GLsizei height);
    void renderbufferStorageMultisample(GLenum target, GLsizei samples, GLenum internalformat,
                                        GLsizei width, GLsizei height);

    // --- Readback ---
    /// readPixels into client memory of `size` bytes: fewer than the read
    /// writes, or a bound PIXEL_PACK_BUFFER, is INVALID_OPERATION.
    void readPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* pixels,
                    size_t size);

    /// Submit everything recorded so far. Never waits.
    void flush();
    /// Submit and wait for this context's work (its own ticket, not the device).
    void finish();

    // --- Transform feedback (webgl_vk_context_feedback.cpp) ---
    /// Whether the device can capture (its vertex stage can store); without
    /// it createTransformFeedback answers 0.
    bool transformFeedbackSupported() const;
    GLuint createTransformFeedback();
    void deleteTransformFeedback(GLuint id);
    GLboolean isTransformFeedback(GLuint id) const;  // true once bound
    void bindTransformFeedback(GLenum target, GLuint id);
    GLuint boundTransformFeedback() const { return boundFeedback_; }
    void beginTransformFeedback(GLenum primitiveMode);
    void endTransformFeedback();
    void pauseTransformFeedback();
    void resumeTransformFeedback();
    bool transformFeedbackActive() const;
    bool transformFeedbackPaused() const;
    void transformFeedbackVaryings(GLuint program, const std::vector<std::string>& varyings, GLenum bufferMode);
    bool getTransformFeedbackVarying(GLuint program, GLuint index, VkFeedbackVarying& out);
    /// The buffer at an indexed binding point (TRANSFORM_FEEDBACK_BUFFER or
    /// UNIFORM_BUFFER), 0 for none.
    GLuint indexedBuffer(GLenum target, GLuint index);

    // --- Query objects (webgl_vk_context_queries.cpp) ---
    GLuint createQuery();
    void deleteQuery(GLuint id);
    GLboolean isQuery(GLuint id) const;  // true once begun
    void beginQuery(GLenum target, GLuint id);
    void endQuery(GLenum target);
    GLuint currentQuery(GLenum target);  // getQuery(target, CURRENT_QUERY)
    /// QUERY_RESULT (waiting for the GPU if need be) or QUERY_RESULT_AVAILABLE;
    /// false with the GL error.
    bool getQueryParameter(GLuint id, GLenum pname, GLuint& out);

    // --- Sync objects (webgl_vk_context_sync.cpp) ---
    /// MAX_CLIENT_WAIT_TIMEOUT_WEBGL: longer client waits are refused.
    static constexpr double kMaxClientWaitTimeoutNs = 1e9;
    GLuint fenceSync(GLenum condition, GLbitfield flags);
    void deleteSync(GLuint id);
    GLboolean isSync(GLuint id) const;
    GLenum clientWaitSync(GLuint id, GLbitfield flags, double timeoutNs);
    void waitSync(GLuint id, GLbitfield flags, double timeoutNs);
    /// False (with the GL error) for a deleted sync or an unknown pname.
    bool getSyncParameter(GLuint id, GLenum pname, GLint& out);

private:
    /// True when the device has every one of `features` for `format`
    /// (optimal tiling): the extensions it can honestly expose.
    bool formatSupports(VkFormat format, VkFormatFeatureFlags features) const;
    /// EXT_texture_filter_anisotropic: the device filters anisotropically.
    bool anisotropicFiltering() const;
    /// The compressed-texture extensions this device backs, and enabling one.
    std::vector<std::string> compressedTextureExtensions() const;
    bool enableCompressedExtension(const std::string& name);
    bool markBufferBound(GLuint id);
    GLuint getBoundBufferId(GLenum target) const;
    /// Bytes readPixels writes for a width x height rectangle of
    /// format/type under the PACK_* state, or 0 when the combination is not
    /// one WebGL2 lets this read buffer be read as (with the GL error set).
    size_t readPixelsByteCount(GLsizei width, GLsizei height, GLenum format, GLenum type);
    /// readPixels into memory known to hold the result.
    void readPixelsInto(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type,
                        void* pixels);
    /// readPixels into a pack buffer as a GPU copy, when format/type are the
    /// read buffer's own bytes and the layout is one a buffer-image copy can
    /// express. False (nothing recorded) otherwise.
    bool readPixelsCopyToBuffer(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format,
                                GLenum type, VkBufferResource& pbo, GLintptr offset);
    std::unordered_map<GLuint, uint64_t> syncs_;  // sync object -> the queue ticket it signals with

    void initVulkanResources();
    void cleanupVulkanResources();

    // Command stream (webgl_vk_context_commands.cpp). All GPU work — draws,
    // clears, uploads, copies, blits, layout changes — is recorded in API order
    // into the stream's open command buffer, so ordering between, say, a
    // bufferSubData and the draws around it is the command stream's. A flush
    // submits it without waiting; only readbacks and client waits block, on
    // this context's own ticket. Open work is submitted at frame end at the
    // latest (a VulkanFrames frame-end hook), and earlier whenever the stream's
    // segment has used its memory budget (flushIfOverBudget, called where no
    // upload slice or descriptor set of the open segment is still held).
    VkCommandBuffer commands();
    VkCommandBuffer transferCommands();  // commands() outside dynamic rendering
    void beginRendering();
    void endRendering();
    void flushCommands();
    void flushIfOverBudget();
    bool waitForCommands();
    /// Copy `size` bytes into the open segment's upload memory.
    render::UploadSlice stage(const void* data, VkDeviceSize size, VkDeviceSize alignment = 16);
    /// Record a copy of `data` into `res` at `offset`, ordered after earlier
    /// GPU use of the buffer and before later use.
    void uploadToBuffer(VkBufferResource& res, VkDeviceSize offset, const void* data, VkDeviceSize size);
    /// Record a transition of every subresource of `tex` to `layout`.
    void transitionTexture(VkCommandBuffer cmd, VkTextureResource& tex, VkImageLayout layout);
    /// Destroy GPU objects once the GPU is done with them.
    void releaseBuffer(VkBufferResource& res);
    void releaseTexture(VkTextureResource& tex);
    /// Host-visible memory of at least `size` bytes that a copy recorded into
    /// the command stream can land in, for readbacks (valid until the next).
    void* readbackMemory(VkDeviceSize size);
    struct Readback {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;
        uint64_t allocId = 0;
        void* mapped = nullptr;
        VkDeviceSize size = 0;
    };
    Readback readback_;

    // Framebuffers (webgl_vk_context_framebuffers.cpp, _blit.cpp, _clear.cpp,
    // _readback.cpp). A Surface is one image a framebuffer reads or writes:
    // a texture level/layer, a renderbuffer, or the canvas's color or depth
    // image. Textures stay sampleable between passes (a pass moves its
    // attachments to attachment layout and back); renderbuffers and the
    // canvas stay wherever they were last used.
    struct Surface {
        enum class Source : uint8_t { None, Texture, Renderbuffer, CanvasColor, CanvasDepth };
        Source source = Source::None;
        VkTextureResource* tex = nullptr;   // Texture / Renderbuffer storage
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;  // the attached level + layer, every aspect
        VkFormat format = VK_FORMAT_UNDEFINED;
        uint32_t width = 0, height = 0;     // of the attached level
        uint32_t level = 0, layer = 0;      // the subresource (a 3D texture's layer is 0 ...
        int32_t z = 0;                      // ... and its slice is z)
        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
        bool alphaOne = false;              // an RGB format stored as RGBA: alpha stays one
        explicit operator bool() const { return source != Source::None; }
        // The canvas is stored top-down; framebuffer objects in GL row order.
        bool topDown() const { return source == Source::CanvasColor || source == Source::CanvasDepth; }
        VkImageAspectFlags aspects() const;
    };
    // What a pass over the draw framebuffer renders into: color[i] is what
    // fragment output i writes (DRAW_BUFFERi), empty for NONE.
    struct RenderTarget {
        std::array<Surface, 8> color{};
        uint32_t colorCount = 0;
        Surface depth;    // the depth attachment, when it has a depth aspect
        Surface stencil;  // the stencil attachment, when it has a stencil aspect
        VkExtent2D extent{};
        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
        bool topDown = false;
    };
    static constexpr GLenum kColorAttachment0 = 0x8CE0;
    VkFramebufferResource* framebufferForTarget(GLenum target, GLuint& id);
    GLenum framebufferStatus(GLuint id);
    Surface attachmentSurface(const VkFboAttachment& att);
    Surface canvasSurface(bool depth);
    VkImageView attachmentView(VkTextureResource& tex, uint32_t level, uint32_t layer);
    /// The draw framebuffer as a render target; false (with
    /// INVALID_FRAMEBUFFER_OPERATION) when it is incomplete.
    bool drawTarget(RenderTarget& out);
    /// The read framebuffer's color read buffer (empty for NONE); false (with
    /// INVALID_FRAMEBUFFER_OPERATION) when it is incomplete.
    bool readColorSurface(Surface& out);
    void transitionSurface(VkCommandBuffer cmd, const Surface& s, VkImageLayout layout);
    VkImageLayout surfaceLayout(const Surface& s) const;
    void setAttachment(GLenum target, GLenum attachment, const VkFboAttachment& att);
    /// End the open pass when framebuffer `id` is what it renders into.
    void framebufferChanged(GLuint id);
    /// A single-sample image a resolve lands in, destroyed once the GPU is done.
    bool scratchImage(VkFormat format, uint32_t width, uint32_t height, VkTextureResource& out);
    void releaseScratch(VkTextureResource& tex);
    void clearAttachments(const VkClearAttachment* atts, uint32_t count);

    // What one draw fetches: vertices [first, end) of the per-vertex arrays
    // for `instances` instances, and whether its indices restart primitives.
    struct DrawShape {
        uint32_t first = 0, end = 0;
        uint32_t instances = 1;
        bool restart = false;
        bool feedback = false;      // captures (transform feedback active)
        FeedbackPush feedbackPush;  // where its records go
    };

    // An indexed buffer binding; size 0 is bindBufferBase's whole buffer.
    struct IndexedBuffer {
        GLuint buffer = 0;
        GLintptr offset = 0;
        GLsizeiptr size = 0;
    };

    // Transform feedback objects (webgl_vk_context_feedback.cpp); 0 is the
    // default one.
    struct FeedbackObject {
        std::array<IndexedBuffer, kMaxFeedbackBuffers> buffers{};
        bool everBound = false;
        bool active = false, paused = false;
        GLenum primitiveMode = 0;
        GLuint program = 0;    // the program capturing
        uint32_t records = 0;  // vertices captured since beginTransformFeedback
    };
    std::unordered_map<GLuint, FeedbackObject> feedbacks_{{0, FeedbackObject{}}};
    GLuint boundFeedback_ = 0;
    VkBufferResource feedbackPlaceholder_;  // bound while a capturing program is not capturing
    bool feedbackCapturing() const;
    void feedbackWritesDone();
    bool bindFeedbackBuffer(GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size);
    bool prepareFeedback(GLenum mode, uint32_t count, uint32_t instances, DrawShape& shape);
    void feedbackDrawn(GLenum mode, const DrawShape& shape);
    void feedbackDescriptors(const VkProgramResource& prog, DrawShape& shape,
                             std::vector<VkDescriptorBufferInfo>& out);
    void destroyFeedback();
    /// Read back a buffer the GPU wrote before its CPU copy is read.
    bool syncShadow(VkBufferResource& res);

    // Queries (webgl_vk_context_queries.cpp)
    struct QueryObject {
        GLenum target = 0;              // 0 until first begun
        std::vector<uint32_t> slots;    // the occlusion slots of the last run
        uint64_t ticket = 0;            // the submission its last use is in
        bool pending = false;           // used in the open command buffer
        uint64_t count = 0;             // samples (summed) or primitives written
    };
    std::unordered_map<GLuint, QueryObject> queries_;
    GLuint activeOcclusionQuery_ = 0;
    GLuint activeFeedbackQuery_ = 0;
    std::vector<VkQueryPool> queryPools_;
    std::vector<uint32_t> freeQuerySlots_;
    std::vector<std::pair<uint64_t, std::vector<uint32_t>>> retiredQuerySlots_;
    std::vector<GLuint> unsubmittedQueries_;
    int32_t openQuerySlot_ = -1;        // the slot open in the current pass
    bool occlusionSuspended_ = false;   // a pass that must not count (an internal clear's)
    void retireQuerySlots(QueryObject& q);
    void queriesSubmitted();
    void queryUsed(QueryObject& q, GLuint id);
    bool allocQuerySlot(VkCommandBuffer cmd, uint32_t& slot);
    void prepareOcclusionSlot(VkCommandBuffer cmd);
    void beginOcclusionSlot(VkCommandBuffer cmd);
    void endOcclusionSlot(VkCommandBuffer cmd);
    void destroyQueries();

    // Clears under partial write masks (webgl_vk_context_maskedclear.cpp).
    struct MaskedClear {
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkShaderModule vertex = VK_NULL_HANDLE;
        std::unordered_map<std::string, VkShaderModule> fragments;  // by written attachments' kinds
    };
    MaskedClear maskedClear_;
    bool maskedClearResources();
    VkShaderModule maskedClearShader(uint32_t colorMask);
    void destroyMaskedClear();
    void clearMasked(uint32_t colorMask, const uint32_t color[4], bool stencil, int32_t stencilValue);
    void implementationReadFormat(const Surface& s, GLenum& format, GLenum& type) const;
    RenderTarget pass_;   // the open pass's target (while inRenderPass_)

    // Textures (webgl_vk_context_textures.cpp: objects, parameters,
    // completeness; _texstorage.cpp: storage and uploads; _texcopy.cpp:
    // compressed images, copies from the read framebuffer, mipmaps;
    // _samplers.cpp: sampler objects, views and the sampler cache).
    /// The texture bound to `target` on the active unit (a cube face names
    /// the cube map), or null with the GL error: INVALID_ENUM for a target
    /// `allowed` does not name, INVALID_OPERATION when none is bound.
    enum TargetSet : uint8_t { k2DTargets = 1, k3DTargets = 2, kTextureTargets = 4 };
    VkTextureResource* textureForTarget(GLenum target, uint8_t allowed);
    /// Client pixels an upload reads: bytes of (format, type) under the
    /// unpack state, or a decoded DOM source (RGBA8, alignment 1).
    struct PixelSource {
        const void* data = nullptr;
        size_t size = 0;
        GLenum format = 0, type = 0;
        bool dom = false;
        uint32_t domWidth = 0, domHeight = 0;
        bool pbo = false;  // from PIXEL_UNPACK_BUFFER (no flip / premultiply)
        bool raw = false;  // tightly packed, no unpack state (internal copies)
    };
    UnpackState unpackState(const PixelSource& src) const;
    /// Validation shared by texImage* / texStorage*: the level's size
    /// against the limits of `target`. False with the GL error.
    bool validateLevelSize(GLenum target, GLint level, GLsizei width, GLsizei height, GLsizei depth);
    /// Give `level` (layer `face` of a cube map) the format and size of a
    /// texImage*, reallocating the image when it cannot hold them.
    bool defineLevel(VkTextureResource& tex, uint32_t level, uint32_t face, const TexFormat& tf, uint32_t width,
                     uint32_t height, uint32_t depth);
    /// Fresh storage: a mip chain of `levels` (0: full) whose level 0 is
    /// width x height x depth, keeping the levels of the old image that are
    /// the same size and format in the new one.
    bool allocateTexture(VkTextureResource& tex, const TexFormat& tf, uint32_t width, uint32_t height,
                         uint32_t depth, uint32_t levels);
    void texImage(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
                  GLsizei depth, GLint border, const PixelSource& src, bool is3D);
    void texSubImage(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width,
                     GLsizei height, GLsizei depth, const PixelSource& src, bool is3D);
    bool pboSource(GLintptr offset, PixelSource& src);
    /// Unpack `src` into a region of one level (layers or 3D slices
    /// [z, z + depth)) through the command stream.
    void writeTexels(VkTextureResource& tex, uint32_t level, int32_t x, int32_t y, int32_t z, uint32_t width,
                     uint32_t height, uint32_t depth, const PixelSource& src);
    void zeroTexels(VkTextureResource& tex, uint32_t level, uint32_t layer, uint32_t layerCount);
    void compressedImage(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height,
                         GLsizei depth, GLint border, const void* data, size_t size, bool is3D);
    void compressedSubImage(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset,
                            GLsizei width, GLsizei height, GLsizei depth, GLenum format, const void* data,
                            size_t size, bool is3D);
    bool compressedFormatEnabled(GLenum internalformat) const;
    std::vector<std::string> enabledCompressedExtensions_;
    bool writeCompressed(VkTextureResource& tex, uint32_t level, int32_t x, int32_t y, int32_t z, uint32_t width,
                         uint32_t height, uint32_t depth, const void* data, size_t size);
    void copyFromReadBuffer(VkTextureResource& tex, uint32_t level, uint32_t layer, int32_t xoffset,
                            int32_t yoffset, GLint x, GLint y, GLsizei width, GLsizei height);
    /// Record transitions of `levelCount` levels x `layerCount` layers of
    /// `tex` (a 3D texture's layer 0 is every slice) to `layout`.
    void transitionTextureRange(VkCommandBuffer cmd, VkTextureResource& tex, uint32_t level,
                                uint32_t levelCount, uint32_t layer, uint32_t layerCount, VkImageLayout layout);
    /// Sampling: whether `tex` read through `state` is complete, and the
    /// level range a sampler then reads (ES 3.0 3.8.13).
    bool textureComplete(const VkTextureResource& tex, const SamplerState& state, uint32_t& base,
                         uint32_t& count) const;
    VkImageView sampledView(VkTextureResource& tex, uint32_t base, uint32_t count);
    VkSampler samplerFor(const SamplerState& state, const TexFormat& tf, bool mipmapped);
    struct SamplerKey {
        uint32_t bits = 0;
        float minLod = 0.0f, maxLod = 0.0f, anisotropy = 0.0f;
        bool operator<(const SamplerKey& o) const {
            return std::tie(bits, minLod, maxLod, anisotropy) < std::tie(o.bits, o.minLod, o.maxLod, o.anisotropy);
        }
    };
    std::map<SamplerKey, VkSampler> samplerCache_;
    void destroySamplerCache();
    bool setSamplerParameter(SamplerState& state, GLenum pname, GLint i, GLfloat f, bool isFloat);

    // Programs (webgl_vk_context_program.cpp, _introspection.cpp, _uniforms.cpp)
    const VkProgramResource* linkedProgram(WebGLProgram p) const;
    bool buildProgramLayouts(VkProgramResource& prog);
    void releaseProgramExecutable(VkProgramResource& prog);
    void destroyProgram(GLuint id);
    enum class UniformKind { Float, Int, Uint };
    VkUniformInfo* uniformTarget(WebGLUniformLocation loc, GLsizei count, VkProgramResource*& prog,
                                 uint32_t& element);
    void setUniformValues(WebGLUniformLocation loc, GLsizei count, UniformKind kind, uint32_t components,
                          const void* data);
    void setUniformMatrices(WebGLUniformLocation loc, GLsizei count, uint32_t columns, uint32_t rows,
                            GLboolean transpose, const GLfloat* value);


    // Drawing (webgl_vk_context_draw.cpp)
    bool prepareDraw(GLenum mode, VkProgramResource& prog, DrawShape& shape);
    int32_t scissorTop(VkExtent2D extent) const;
    void buildPipelineKey(GLenum mode, const VkProgramResource& prog, const DrawShape& shape, PipelineKey& key,
                          VkExtent2D& extent);
    void setDynamicState(VkCommandBuffer cmd, VkExtent2D extent);
    bool bindProgramResources(VkCommandBuffer cmd, VkProgramResource& prog, DrawShape& shape);
    GLuint textureForSampler(GLenum type, uint32_t unit) const;
    bool sampledByPass(const VkTextureResource& tex, uint32_t base, uint32_t count) const;
    bool uniformBlockRange(const VkProgramResource& prog, size_t block, VkDescriptorBufferInfo& out);
    VkProgramResource* drawProgram(const char* what);
    bool samplerUnitConflict(const VkProgramResource& prog) const;

    // Draw calls (webgl_vk_context_drawcalls.cpp)
    bool drawModeValid(GLenum mode);
    bool indexRange(VkBufferResource& ibo, uintptr_t offset, uint32_t count, GLenum type, IndexRange& out);
    void drawIndices(GLenum mode, VkProgramResource& prog, VkBuffer buffer, VkDeviceSize offset,
                     VkIndexType type, uint32_t count, DrawShape& shape);

    // Vertex inputs (webgl_vk_context_vertices.cpp)
    /// The program's inputs as vertex bindings: false (with the GL error)
    /// when an array the draw reads is missing, of the wrong kind, or too
    /// short for the vertices it fetches.
    bool bindVertexInputs(const VkProgramResource& prog, const DrawShape& shape, PipelineKey& key,
                          std::vector<VkBuffer>& vbos, std::vector<VkDeviceSize>& offsets);
    bool vertexFormatSupported(VkFormat format);
    std::unordered_map<VkFormat, bool> vertexFormatSupport_;

    render::VulkanContext& context_;
    WebGLVkStream stream_;
    WebGLVkCanvas canvas_;
    WebGLVkPipelineCache pipelineCache_;

    bool inRenderPass_ = false;
    render::VulkanFrames::HookId frameEndHook_ = 0;

    // Sampled in place of a missing or incomplete texture, which GL reads as
    // (0, 0, 0, 1): one per component kind (float, int, uint, and depth for
    // shadow samplers, which compare to 0), each with a 2D, 2D-array, cube
    // and (but depth) 3D view.
    struct Placeholder {
        VkImage layered = VK_NULL_HANDLE;  // six layers, cube compatible
        VkDeviceMemory layeredMemory = VK_NULL_HANDLE;
        VkImage volume = VK_NULL_HANDLE;
        VkDeviceMemory volumeMemory = VK_NULL_HANDLE;
        std::array<VkImageView, 4> views{};  // 2D, 2D array, cube, 3D
    };
    std::array<Placeholder, 4> placeholders_{};  // float, int, uint, depth
    VkSampler placeholderSampler_ = VK_NULL_HANDLE;
    void createPlaceholders();
    void destroyPlaceholders();
    VkImageView placeholderView(GLenum samplerType) const;

    VkSampler placeholderShadowSampler_ = VK_NULL_HANDLE;

    // State tracking
    VkViewport viewport_{};
    VkRect2D scissor_{};
    bool scissorTest_ = false;

    float clearColor_[4]{0.0f, 0.0f, 0.0f, 0.0f};
    float clearDepth_ = 1.0f;
    int32_t clearStencil_ = 0;

    bool blendEnabled_ = false;
    bool depthTestEnabled_ = false;
    bool cullFaceEnabled_ = false;
    GLenum depthFunc_ = GL_LESS;
    GLboolean depthMask_ = GL_TRUE;
    GLenum cullFaceMode_ = GL_BACK;
    GLenum frontFaceMode_ = GL_CCW;
    GLenum blendSrcRGB_ = GL_ONE, blendDstRGB_ = GL_ZERO;
    GLenum blendSrcAlpha_ = GL_ONE, blendDstAlpha_ = GL_ZERO;
    GLenum blendEqRGB_ = GL_FUNC_ADD, blendEqAlpha_ = GL_FUNC_ADD;
    GLboolean colorMask_[4]{GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};

    bool stencilTestEnabled_ = false;
    GLenum stencilFuncFront_ = GL_ALWAYS;
    GLint stencilRefFront_ = 0;
    GLuint stencilValueMaskFront_ = 0xFFFFFFFF;
    GLenum stencilFailFront_ = GL_KEEP;
    GLenum stencilPassDepthFailFront_ = GL_KEEP;
    GLenum stencilPassDepthPassFront_ = GL_KEEP;
    GLuint stencilWriteMaskFront_ = 0xFFFFFFFF;

    GLenum stencilFuncBack_ = GL_ALWAYS;
    GLint stencilRefBack_ = 0;
    GLuint stencilValueMaskBack_ = 0xFFFFFFFF;
    GLenum stencilFailBack_ = GL_KEEP;
    GLenum stencilPassDepthFailBack_ = GL_KEEP;
    GLenum stencilPassDepthPassBack_ = GL_KEEP;
    GLuint stencilWriteMaskBack_ = 0xFFFFFFFF;

    GLenum pendingError_ = GL_NO_ERROR;

    // Resource registries
    std::unordered_map<GLuint, VkBufferResource> buffers_;
    std::unordered_map<GLuint, VkTextureResource> textures_;
    std::unordered_map<GLuint, VkShaderResource> shaders_;
    std::unordered_map<GLuint, VkProgramResource> programs_;
    std::unordered_map<GLuint, VkVAOResource> vaos_;
    std::unordered_map<GLuint, VkFramebufferResource> framebuffers_;
    std::unordered_map<GLuint, VkRenderbufferResource> renderbuffers_;
    std::unordered_map<GLuint, VkSamplerResource> samplers_;

    // Object names, one sequence for every kind. A context restored after a
    // loss continues its predecessor's, so no name of a lost object ever
    // names a new one.
    GLuint nextObjectId_ = 1;

    GLuint readFboId_ = 0;   // READ_FRAMEBUFFER binding (0 = the canvas)
    GLuint drawFboId_ = 0;   // DRAW_FRAMEBUFFER binding
    GLuint currentRenderbufferId_ = 0;
    // The canvas's DRAW_BUFFER0 / READ_BUFFER: BACK or NONE.
    GLenum canvasDrawBuffer_ = GL_BACK;
    GLenum canvasReadBuffer_ = GL_BACK;

    GLuint boundArrayBuffer_ = 0;
    GLuint boundElementArrayBuffer_ = 0;
    GLuint boundPixelPackBuffer_ = 0;
    GLuint boundPixelUnpackBuffer_ = 0;
    GLuint boundUniformBuffer_ = 0;
    GLuint boundCopyReadBuffer_ = 0;
    GLuint boundCopyWriteBuffer_ = 0;
    GLuint boundTransformFeedbackBuffer_ = 0;
    GLuint currentProgramId_ = 0;
    GLuint currentVaoId_ = 0;

    UnpackState unpack_;  // UNPACK_* except the WebGL flags below
    PackState pack_;
    bool unpackFlipY_ = false;
    bool unpackPremultiplyAlpha_ = false;
    GLint unpackColorspaceConversion_ = 0x9244; // BROWSER_DEFAULT_WEBGL
    bool ditherEnabled_ = false;
    bool polygonOffsetFillEnabled_ = false;
    bool rasterizerDiscardEnabled_ = false;
    bool sampleAlphaToCoverageEnabled_ = false;
    bool sampleCoverageEnabled_ = false;
    GLfloat sampleCoverageValue_ = 1.0f;
    bool sampleCoverageInvert_ = false;
    GLfloat depthNear_ = 0.0f, depthFar_ = 1.0f;
    GLfloat blendColor_[4]{0.0f, 0.0f, 0.0f, 0.0f};
    GLfloat lineWidth_ = 1.0f;
    GLfloat polygonOffsetFactor_ = 0.0f, polygonOffsetUnits_ = 0.0f;
    GLenum generateMipmapHint_ = GL_DONT_CARE, derivativeHint_ = GL_DONT_CARE;

    // Constant values of disabled vertex attributes, bound as a zero-stride
    // vertex buffer from the upload ring (re-staged whenever they change).
    std::array<std::array<uint32_t, 4>, 16> genericAttribs_{};
    std::array<VkVertexInput::Kind, 16> genericAttribKinds_{};  // of the last vertexAttrib* call
    render::UploadSlice genericAttribSlice_;
    uint64_t genericAttribSerial_ = 0;
    void genericAttribsChanged() { genericAttribSerial_ = 0; }

    // Indexed UNIFORM_BUFFER bindings (IndexedBuffer, above).
    static constexpr uint32_t kMaxUniformBufferBindings = 36;
    std::array<IndexedBuffer, kMaxUniformBufferBindings> boundUniformBuffers_{};

    GLuint activeTextureUnit_ = 0;
    std::array<GLuint, 32> boundTextures2D_{};
    std::array<GLuint, 32> boundTexturesCubeMap_{};
    std::array<GLuint, 32> boundTextures2DArray_{};
    std::array<GLuint, 32> boundTextures3D_{};
    std::array<GLuint, 32> boundSamplers_{};
};

} // namespace bro::webgl::vk
