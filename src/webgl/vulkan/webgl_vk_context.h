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
    void cullFace(GLenum mode);
    void frontFace(GLenum mode);
    void lineWidth(GLfloat width);
    void polygonOffset(GLfloat factor, GLfloat units);
    GLenum getError();
    void setSyntheticError(GLenum err);

    // --- Buffers ---
    WebGLBuffer createBuffer();
    void deleteBuffer(WebGLBuffer buf);
    void bindBuffer(GLenum target, WebGLBuffer buf);
    void bufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage);
    void bufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void* data);
    void getBufferSubData(GLenum target, GLintptr srcByteOffset, void* dstData, GLsizeiptr length);
    void* mapBufferRange(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access);
    bool unmapBuffer(GLenum target);
    void flushMappedBufferRange(GLenum target, GLintptr offset, GLsizeiptr length);
    GLuint boundBuffer(GLenum target);

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

    // --- Uniforms & Attributes ---
    GLint getAttribLocation(WebGLProgram p, const std::string& name);
    WebGLUniformLocation getUniformLocation(WebGLProgram p, const std::string& name);
    void uniform1f(WebGLUniformLocation loc, GLfloat v0);
    void uniform2f(WebGLUniformLocation loc, GLfloat v0, GLfloat v1);
    void uniform3f(WebGLUniformLocation loc, GLfloat v0, GLfloat v1, GLfloat v2);
    void uniform4f(WebGLUniformLocation loc, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3);
    void uniform1i(WebGLUniformLocation loc, GLint v0);
    void uniform2i(WebGLUniformLocation loc, GLint v0, GLint v1);
    void uniform1fv(WebGLUniformLocation loc, GLsizei count, const GLfloat* v);
    void uniform2fv(WebGLUniformLocation loc, GLsizei count, const GLfloat* v);
    void uniform3fv(WebGLUniformLocation loc, GLsizei count, const GLfloat* v);
    void uniform4fv(WebGLUniformLocation loc, GLsizei count, const GLfloat* v);
    void uniformMatrix3fv(WebGLUniformLocation loc, GLsizei count, GLboolean transpose, const GLfloat* value);
    void uniformMatrix4fv(WebGLUniformLocation loc, GLsizei count, GLboolean transpose, const GLfloat* value);

    // --- Vertex Arrays (VAO) ---
    WebGLVertexArrayObject createVertexArray();
    void deleteVertexArray(WebGLVertexArrayObject vao);
    void bindVertexArray(WebGLVertexArrayObject vao);
    void enableVertexAttribArray(GLuint index);
    void disableVertexAttribArray(GLuint index);
    void vertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized,
                             GLsizei stride, uintptr_t offset);
    void vertexAttribDivisor(GLuint index, GLuint divisor);

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
    void generateMipmap(GLenum target);

    // --- Framebuffers ---
    WebGLFramebuffer createFramebuffer();
    void deleteFramebuffer(WebGLFramebuffer fb);
    void bindFramebuffer(GLenum target, WebGLFramebuffer fb);
    void framebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget,
                              WebGLTexture tex, GLint level);
    GLenum checkFramebufferStatus(GLenum target);

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

    GLenum pendingError_ = GL_NO_ERROR;

    // Resource registries
    std::unordered_map<GLuint, VkBufferResource> buffers_;
    std::unordered_map<GLuint, VkTextureResource> textures_;
    std::unordered_map<GLuint, VkShaderResource> shaders_;
    std::unordered_map<GLuint, VkProgramResource> programs_;
    std::unordered_map<GLuint, VkVAOResource> vaos_;
    std::unordered_map<GLuint, VkFramebufferResource> framebuffers_;

    GLuint nextBufferId_ = 1;
    GLuint nextTextureId_ = 1;
    GLuint nextShaderId_ = 1;
    GLuint nextProgramId_ = 1;
    GLuint nextVaoId_ = 1;
    GLuint nextFboId_ = 1;

    GLuint boundArrayBuffer_ = 0;
    GLuint boundElementArrayBuffer_ = 0;
    GLuint currentProgramId_ = 0;
    GLuint currentVaoId_ = 0;
    GLuint currentFboId_ = 0;

    GLuint activeTextureUnit_ = 0;
    std::array<GLuint, 32> boundTextures2D_{};
};

} // namespace bro::webgl::vk
