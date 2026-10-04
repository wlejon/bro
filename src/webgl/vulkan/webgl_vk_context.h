#pragma once

#include "render/vulkan_context.h"
#include "webgl/webgl_objects.h"
#include "webgl/vulkan/webgl_vk_types.h"
#include "webgl/vulkan/webgl_vk_canvas.h"
#include "webgl/vulkan/webgl_vk_shaders.h"
#include "webgl/vulkan/webgl_vk_pipeline.h"

#include <vulkan/vulkan.h>
#include "webgl/webgl_types.h"
#include <vector>
#include <unordered_map>
#include <memory>
#include <string>

namespace bro::webgl::vk {

/// Vulkan-backed implementation of WebGL2 rendering commands.
class WebGLVkContext {
public:
    WebGLVkContext(int width, int height, render::VulkanContext& context);
    ~WebGLVkContext();

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
    GLenum getError();
    void setSyntheticError(GLenum err);
    void pixelStorei(GLenum pname, GLint param);
    GLint getPixelStorei(GLenum pname) const;

    GLint getParameterInt(GLenum pname);
    GLfloat getParameterFloat(GLenum pname);
    GLboolean getParameterBool(GLenum pname);
    void getParameterInt2(GLenum pname, GLint* out);
    void getParameterInt4(GLenum pname, GLint* out);
    void getParameterFloat2(GLenum pname, GLfloat* out);
    void getParameterFloat4(GLenum pname, GLfloat* out);
    void getParameterBool4(GLenum pname, GLboolean* out);

    // --- Buffers ---
    WebGLBuffer createBuffer();
    void deleteBuffer(WebGLBuffer buf);
    void bindBuffer(GLenum target, WebGLBuffer buf);
    void bindBufferBase(GLenum target, GLuint index, WebGLBuffer buf);
    void bindBufferRange(GLenum target, GLuint index, WebGLBuffer buf, GLintptr offset, GLsizeiptr size);
    void bufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage);
    void bufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void* data);
    void getBufferSubData(GLenum target, GLintptr srcByteOffset, void* dstData, GLsizeiptr length);
    void copyBufferSubData(GLenum readTarget, GLenum writeTarget, GLintptr readOffset, GLintptr writeOffset, GLsizeiptr size);
    void* mapBufferRange(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access);
    bool unmapBuffer(GLenum target);
    void flushMappedBufferRange(GLenum target, GLintptr offset, GLsizeiptr length);
    GLuint boundBuffer(GLenum target);
    GLuint getBoundBufferId(GLenum target) const;
    int64_t boundBufferSize(GLenum target);

    void texImage2DFromPBO(GLenum target, GLint level, GLint internalformat,
                           GLsizei width, GLsizei height, GLint border,
                           GLenum format, GLenum type, GLintptr offset);
    void texSubImage2DFromPBO(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                              GLsizei width, GLsizei height, GLenum format, GLenum type, GLintptr offset);
    void readPixelsToPBO(GLint x, GLint y, GLsizei width, GLsizei height,
                         GLenum format, GLenum type, GLintptr offset);

    // --- Shaders & Programs ---
    WebGLShader createShader(GLenum type);
    void deleteShader(WebGLShader s);
    void shaderSource(WebGLShader s, const std::string& src);
    void compileShader(WebGLShader s);
    GLint getShaderParameter(WebGLShader s, GLenum pname);
    std::string getShaderInfoLog(WebGLShader s);

    WebGLProgram createProgram();
    void deleteProgram(WebGLProgram p);
    void attachShader(WebGLProgram p, WebGLShader s);
    void detachShader(WebGLProgram p, WebGLShader s);
    void linkProgram(WebGLProgram p);
    void useProgram(WebGLProgram p);
    GLint getProgramParameter(WebGLProgram p, GLenum pname);
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

    // --- Vertex Arrays (VAO) ---
    WebGLVertexArrayObject createVertexArray();
    void deleteVertexArray(WebGLVertexArrayObject vao);
    void bindVertexArray(WebGLVertexArrayObject vao);
    void enableVertexAttribArray(GLuint index);
    void disableVertexAttribArray(GLuint index);
    void vertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized,
                             GLsizei stride, uintptr_t offset);
    void vertexAttribIPointer(GLuint index, GLint size, GLenum type,
                              GLsizei stride, uintptr_t offset);
    void vertexAttribDivisor(GLuint index, GLuint divisor);

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

    // --- Textures ---
    WebGLTexture createTexture();
    void deleteTexture(WebGLTexture tex);
    void bindTexture(GLenum target, WebGLTexture tex);
    void activeTexture(GLenum texture);
    void texParameteri(GLenum target, GLenum pname, GLint param);
    void texImage2D(GLenum target, GLint level, GLint internalformat,
                    GLsizei width, GLsizei height, GLint border,
                    GLenum format, GLenum type, const void* pixels);
    void texSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                       GLsizei width, GLsizei height,
                       GLenum format, GLenum type, const void* pixels);
    void texStorage2D(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height);
    void copyTexImage2D(GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width, GLsizei height, GLint border);
    void copyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width, GLsizei height);
    void texImage3D(GLenum target, GLint level, GLint internalformat,
                    GLsizei width, GLsizei height, GLsizei depth, GLint border,
                    GLenum format, GLenum type, const void* pixels);
    void texSubImage3D(GLenum target, GLint level,
                       GLint xoffset, GLint yoffset, GLint zoffset,
                       GLsizei width, GLsizei height, GLsizei depth,
                       GLenum format, GLenum type, const void* pixels);
    void texStorage3D(GLenum target, GLsizei levels, GLenum internalformat,
                      GLsizei width, GLsizei height, GLsizei depth);
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
    void updateSamplerObject(VkSamplerResource& smp);

    // --- Framebuffers & Renderbuffers ---
    WebGLFramebuffer createFramebuffer();
    void deleteFramebuffer(WebGLFramebuffer fb);
    void bindFramebuffer(GLenum target, WebGLFramebuffer fb);
    void framebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget,
                              WebGLTexture tex, GLint level);
    void framebufferRenderbuffer(GLenum target, GLenum attachment, GLenum renderbuffertarget,
                                 WebGLRenderbuffer rbo);
    GLenum checkFramebufferStatus(GLenum target);
    void drawBuffers(GLsizei n, const GLenum* bufs);
    void readBuffer(GLenum src);
    void blitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1,
                         GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1,
                         GLbitfield mask, GLenum filter);

    WebGLRenderbuffer createRenderbuffer();
    void deleteRenderbuffer(WebGLRenderbuffer rbo);
    void bindRenderbuffer(GLenum target, WebGLRenderbuffer rbo);
    void renderbufferStorage(GLenum target, GLenum internalformat, GLsizei width, GLsizei height);
    void renderbufferStorageMultisample(GLenum target, GLsizei samples, GLenum internalformat,
                                        GLsizei width, GLsizei height);

    // --- Readback ---
    void readPixels(GLint x, GLint y, GLsizei width, GLsizei height,
                    GLenum format, GLenum type, void* pixels);

    void flush();
    void finish();

    void bindCanvasFBO();
    void unbindCanvasFBO();

private:
    void initVulkanResources();
    void cleanupVulkanResources();
    void ensureCommandBuffer();
    void beginRendering();
    void endRendering();
    void submitAndFlush();

    render::VulkanContext& context_;
    WebGLVkCanvas canvas_;
    WebGLVkPipelineCache pipelineCache_;

    // Command recording & dynamic rendering dispatch
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkCommandBuffer currentCmd_ = VK_NULL_HANDLE;
    bool inRenderPass_ = false;

    PFN_vkCmdBeginRenderingKHR pfnCmdBeginRendering_ = nullptr;
    PFN_vkCmdEndRenderingKHR pfnCmdEndRendering_ = nullptr;

    // Descriptors and Layout
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;

    // Dummy fallback 1x1 texture for unbound sampler slots
    VkImage dummyImage_ = VK_NULL_HANDLE;
    VkDeviceMemory dummyMemory_ = VK_NULL_HANDLE;
    VkImageView dummyView_ = VK_NULL_HANDLE;
    VkSampler dummySampler_ = VK_NULL_HANDLE;
    VkBuffer dummyUniformBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory dummyUniformMemory_ = VK_NULL_HANDLE;

    void updateTextureSampler(VkTextureResource& tex);

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

    void updateFallbackConstantBuffer();
    void ensureScratchIndexBufferSize(size_t size);
    void uploadScratchIndexBuffer(const void* data, size_t size);
    VkBuffer scratchIndexBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory scratchIndexMemory_ = VK_NULL_HANDLE;
    size_t scratchIndexSize_ = 0;

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

    GLuint nextBufferId_ = 1;
    GLuint nextTextureId_ = 1;
    GLuint nextShaderId_ = 1;
    GLuint nextProgramId_ = 1;
    GLuint nextVaoId_ = 1;
    GLuint nextFboId_ = 1;
    GLuint nextRenderbufferId_ = 1;
    GLuint nextSamplerId_ = 1;

    GLuint readFboId_ = 0;
    GLuint drawFboId_ = 0;
    GLuint currentRenderbufferId_ = 0;

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
    GLuint currentFboId_ = 0;

    bool unpackFlipY_ = false;
    bool unpackPremultiplyAlpha_ = false;
    GLint unpackColorspaceConversion_ = 0x9244; // BROWSER_DEFAULT_WEBGL
    GLint unpackAlignment_ = 4;
    GLint packAlignment_ = 4;
    bool ditherEnabled_ = false;
    bool polygonOffsetFillEnabled_ = false;
    bool rasterizerDiscardEnabled_ = false;
    bool sampleAlphaToCoverageEnabled_ = false;
    bool sampleCoverageEnabled_ = false;

    std::array<std::array<uint32_t, 4>, 16> genericAttribs_{};
    std::array<GLuint, 32> boundUniformBuffers_{};
    VkBuffer fallbackConstantBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory fallbackConstantMemory_ = VK_NULL_HANDLE;

    GLuint activeTextureUnit_ = 0;
    std::array<GLuint, 32> boundTextures2D_{};
    std::array<GLuint, 32> boundTexturesCubeMap_{};
    std::array<GLuint, 32> boundTextures2DArray_{};
    std::array<GLuint, 32> boundTextures3D_{};
    std::array<GLuint, 32> boundSamplers_{};
};

} // namespace bro::webgl::vk
