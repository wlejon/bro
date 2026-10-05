#pragma once

#include "webgl/webgl_objects.h"
#include "webgl/webgl_types.h"

#include <vulkan/vulkan.h>
#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>

namespace bro::render { class VulkanContext; }
namespace bro::webgl::vk { class WebGLVkContext; }

namespace bro::webgl {


/// The front end's own per-context state, beside the backend's: object
/// tracking, pixel store, the first pending WebGL error, and the bindings it
/// answers getters from. A context restored after a loss starts it again.
struct WebGL2FrontState {
    std::unordered_set<GLuint> validBuffers_;
    std::unordered_set<GLuint> validPrograms_;
    std::unordered_set<GLuint> validShaders_;
    std::unordered_set<GLuint> validFramebuffers_;
    std::unordered_set<GLuint> validRenderbuffers_;
    std::unordered_set<GLuint> validVAOs_;
    std::unordered_set<GLuint> validSamplers_;
    std::unordered_set<GLsync> validSyncs_;

    // pixelStorei state
    GLint unpackAlignment_ = 4;
    GLint packAlignment_ = 4;
    GLboolean unpackFlipY_ = GL_FALSE;
    GLboolean unpackPremultiplyAlpha_ = GL_FALSE;
    GLint unpackColorspace_ = 0x9244; // BROWSER_DEFAULT_WEBGL

    // First pending WebGL-level error (returned by getError before real GL errors)
    GLenum syntheticError_ = 0; // GL_NO_ERROR

    GLint sViewport_[4] = {0, 0, 0, 0};
    GLint sScissorBox_[4] = {0, 0, 0, 0};
    GLuint sProgram_ = 0;
    GLuint sVAO_ = 0;
    GLuint sArrayBuf_ = 0;
    GLuint sElementBuf_ = 0;
    GLuint sPixelPack_ = 0;
    GLuint sPixelUnpack_ = 0;
    bool sScissorTest_ = false;
};

/// WebGL2RenderingContext — maps WebGL2 API calls onto the Vulkan backend
/// (vk::WebGLVkContext), which owns the offscreen VkImage that serves as the
/// WebGL canvas; the engine composites it into the window.
class WebGL2RenderingContext : private WebGL2FrontState {
public:
    WebGL2RenderingContext(int width, int height, render::VulkanContext* vkContext = nullptr);
    ~WebGL2RenderingContext();

    static void setDefaultVulkanContext(render::VulkanContext* ctx) { defaultVulkanContext_ = ctx; }
    static render::VulkanContext* defaultVulkanContext() { return defaultVulkanContext_; }

    vk::WebGLVkContext* vkContext() const { return vkCtx_.get(); }
    bool isVulkanBackend() const { return vkCtx_ != nullptr; }

    WebGL2RenderingContext(const WebGL2RenderingContext&) = delete;
    WebGL2RenderingContext& operator=(const WebGL2RenderingContext&) = delete;

    using TeardownCallback = std::function<void(WebGL2RenderingContext*)>;
    void addTeardownCallback(TeardownCallback cb) {
        teardownCallbacks_.push_back(std::move(cb));
    }

    /// Resize the canvas.
    void resize(int width, int height);

    int canvasWidth() const { return width_; }
    int canvasHeight() const { return height_; }

    VkImage vkColorImage() const;
    VkImageLayout vkColorLayout() const;

    /// Submit the context's recorded work: the engine is about to composite
    /// or read the canvas.
    void unbindCanvasFBO();

    /// Read the canvas's colour buffer back as tightly packed, top-down RGBA
    /// (canvasWidth() x canvasHeight()), the row order every image encoder and
    /// the 2D canvas surface use — GL's own bottom-up rows are flipped here.
    /// This is what backs toDataURL()/toBlob() on a WebGL canvas. Returns false
    /// if the context has no colour buffer to read. Leaves the app's GL state
    /// as it found it.
    bool readCanvasPixels(std::vector<uint8_t>& out);

    // =================================================================
    // Multiple canvases
    //
    // A document may hold several <canvas> elements with their own WebGL
    // contexts, but they all multiplex one real GL context, so "which
    // context's state is live in the driver" has to be tracked here. Every
    // WebGL call arriving from JS routes through getCtx(), which calls
    // makeCurrent(): if this context is not the live one, its shadow state
    // (FBO, program, VAO, bindings, blend/depth/cull, viewport) is re-applied
    // first. Without it, a second canvas's draw calls land in the first
    // canvas's framebuffer.
    //
    // The engine invalidates the cache around its own GL work (compositing,
    // screenshot readback), since that clobbers state behind every context's
    // back.
    // =================================================================

    /// Make this context's shadow state live in the driver. Cheap no-op when
    /// it already is, which is the overwhelmingly common single-canvas case.
    void makeCurrent();

    /// The context whose state is currently live, or nullptr.
    static WebGL2RenderingContext* current() { return current_; }

    /// Forget which context is live; the next WebGL call re-applies its state.
    /// Call before handing GL to app JS after the engine has used it.
    static void invalidateCurrent() { current_ = nullptr; }

    /// Hand GL back to the engine: neutralise the live context's state and
    /// drop the cache. Safe when no context was touched.
    static void endAppGL() {
        if (current_) current_->unbindCanvasFBO();
        current_ = nullptr;
    }

    // =================================================================
    // WebGL2 API methods
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
    void polygonOffset(GLfloat factor, GLfloat units);
    void lineWidth(GLfloat width);
    void pixelStorei(GLenum pname, GLint param);
    GLenum getError();

    /// Record a WebGL-level (synthetic) error that raw GL cannot produce,
    /// e.g. INVALID_OPERATION for a readPixels destination that is too small.
    /// Mirrors GL semantics: only the first pending error is kept.
    void setSyntheticError(GLenum err);

    // pixelStorei shadow state (WebGL-only pnames are not real GL enums)
    GLboolean unpackFlipY() const { return unpackFlipY_; }
    GLboolean unpackPremultiplyAlpha() const { return unpackPremultiplyAlpha_; }
    GLint unpackColorspaceConversion() const { return unpackColorspace_; }
    GLint packAlignment() const { return packAlignment_; }
    GLint unpackAlignmentValue() const { return unpackAlignment_; }

    // --- Buffers ---
    WebGLBuffer createBuffer();
    void deleteBuffer(WebGLBuffer buf);
    void bindBuffer(GLenum target, WebGLBuffer buf);
    void bufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage);
    void bufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void* data);
    void copyBufferSubData(GLenum readTarget, GLenum writeTarget,
                           GLintptr readOffset, GLintptr writeOffset, GLsizeiptr size);
    void getBufferSubData(GLenum target, GLintptr srcByteOffset, void* dstData, GLsizeiptr length);

    // --- Buffer mapping (BRO_buffer_map; GL 3.0, no WebGL equivalent) ---
    // Hands JS a pointer into driver-owned buffer storage.
    //
    // This is a capability, not a speed-up: measured against bufferSubData it
    // is a wash (1.00x/1.15x/1.00x at 64 KB/1 MB/4 MB), because our
    // bufferSubData already passes the caller's TypedArray straight to GL, so
    // both paths move the bytes exactly once. What mapping buys is
    // read-modify-write of a sub-range with no JS-side mirror of the data, and
    // UNSYNCHRONIZED streaming, neither of which bufferSubData can express.
    //
    // Mappings are keyed by buffer id, not by target: GL ties a mapping to the
    // buffer object, so rebinding the target neither moves nor cancels one,
    // and keying by target would lose track of it.
    void* mapBufferRange(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access);
    /// False if the mapping was lost and its contents must be resubmitted
    /// (glUnmapBuffer's documented GL_FALSE case), or on validation failure.
    bool unmapBuffer(GLenum target);
    void flushMappedBufferRange(GLenum target, GLintptr offset, GLsizeiptr length);
    /// GL id currently bound to `target`, or 0 for an unbound or non-buffer
    /// target. Lets the JS layer key its ArrayBuffer registry the same way.
    GLuint boundBuffer(GLenum target);

    // --- Buffer binding (WebGL2) ---
    void bindBufferBase(GLenum target, GLuint index, WebGLBuffer buf);
    void bindBufferRange(GLenum target, GLuint index, WebGLBuffer buf,
                         GLintptr offset, GLsizeiptr size);

    // --- Sampler objects (WebGL2; ARB_sampler_objects, core in GL 3.3) ---
    WebGLSampler createSampler();
    void deleteSampler(WebGLSampler s);
    void bindSampler(GLuint unit, WebGLSampler s);
    void samplerParameteri(WebGLSampler s, GLenum pname, GLint param);
    void samplerParameterf(WebGLSampler s, GLenum pname, GLfloat param);
    GLint getSamplerParameteri(WebGLSampler s, GLenum pname);
    GLfloat getSamplerParameterf(WebGLSampler s, GLenum pname);
    GLboolean isSampler(WebGLSampler s);

    // --- Sync objects (WebGL2; core since GL 3.2) ---
    /// WebGL2 MAX_CLIENT_WAIT_TIMEOUT_WEBGL — clientWaitSync timeouts above
    /// this (in nanoseconds) raise INVALID_OPERATION instead of blocking the
    /// JS thread indefinitely.
    static constexpr double kMaxClientWaitTimeoutNs = 1e9; // 1 second
    WebGLSync fenceSync(GLenum condition, GLbitfield flags);
    void deleteSync(WebGLSync s);
    GLenum clientWaitSync(WebGLSync s, GLbitfield flags, double timeoutNs);
    void waitSync(WebGLSync s, GLbitfield flags, double timeoutNs);
    GLint getSyncParameter(WebGLSync s, GLenum pname);
    GLboolean isSync(WebGLSync s);

    // --- Query objects (WebGL2) ---
    WebGLQuery createQuery();
    void deleteQuery(WebGLQuery q);
    void beginQuery(GLenum target, WebGLQuery q);
    void endQuery(GLenum target);
    WebGLQuery currentQuery(GLenum target);  // getQuery(target, CURRENT_QUERY)
    /// QUERY_RESULT / QUERY_RESULT_AVAILABLE; false (with the GL error) for
    /// a query that has none.
    bool getQueryParameter(WebGLQuery q, GLenum pname, GLuint& out);
    GLboolean isQuery(WebGLQuery q);

    // --- Transform feedback (WebGL2) ---
    /// False when the device cannot capture (createTransformFeedback then
    /// answers no object).
    bool transformFeedbackSupported() const;
    WebGLTransformFeedback createTransformFeedback();
    void deleteTransformFeedback(WebGLTransformFeedback tf);
    void bindTransformFeedback(GLenum target, WebGLTransformFeedback tf);
    void beginTransformFeedback(GLenum primitiveMode);
    void endTransformFeedback();
    void pauseTransformFeedback();
    void resumeTransformFeedback();
    void transformFeedbackVaryings(WebGLProgram program,
                                   const std::vector<std::string>& varyings,
                                   GLenum bufferMode);
    /// False (with INVALID_VALUE) for an index past the program's varyings.
    bool getTransformFeedbackVarying(WebGLProgram program, GLuint index, WebGLActiveInfo& out);
    GLboolean isTransformFeedback(WebGLTransformFeedback tf);
    bool transformFeedbackActive() const;
    bool transformFeedbackPaused() const;
    WebGLTransformFeedback boundTransformFeedback() const;

    /// getIndexedParameter: the buffer at TRANSFORM_FEEDBACK_BUFFER or
    /// UNIFORM_BUFFER binding `index`, and the numeric rows (*_START, *_SIZE).
    WebGLBuffer indexedBuffer(GLenum target, GLuint index);
    int64_t getIndexedParameterInt64(GLenum pname, GLuint index);

    // --- Pixel buffer objects (WebGL2) ---
    GLuint pixelPackBuffer() const { return sPixelPack_; }
    GLuint pixelUnpackBuffer() const { return sPixelUnpack_; }
    /// Byte size of the buffer bound at `target` (0 when none bound).
    int64_t boundBufferSize(GLenum target);
    /// getBufferParameter / getVertexAttrib / getUniform: false (with the GL
    /// error) for an invalid query.
    bool getBufferParameter(GLenum target, GLenum pname, GLint& out);
    bool getVertexAttrib(GLuint index, GLenum pname, GLValue& out);
    GLintptr getVertexAttribOffset(GLuint index, GLenum pname);
    bool getUniform(WebGLProgram program, WebGLUniformLocation loc, GLValue& out);
    void validateProgram(WebGLProgram program);
    /// readPixels into the bound PIXEL_PACK_BUFFER at a byte offset.
    /// Bounds-checks the offset against the PBO size (same no-overflow
    /// guarantee as the client-memory path) and records synthetic errors.
    void readPixelsToPBO(GLint x, GLint y, GLsizei width, GLsizei height,
                         GLenum format, GLenum type, GLintptr offset);
    /// tex(Sub)Image2D sourcing from the bound PIXEL_UNPACK_BUFFER at a byte
    /// offset. UNPACK_FLIP_Y/PREMULTIPLY_ALPHA are INVALID_OPERATION here
    /// (WebGL2: the transforms only apply to client-memory uploads).
    void texImage2DFromPBO(GLenum target, GLint level, GLint internalformat,
                           GLsizei width, GLsizei height, GLint border,
                           GLenum format, GLenum type, GLintptr offset);
    void texSubImage2DFromPBO(GLenum target, GLint level,
                              GLint xoffset, GLint yoffset,
                              GLsizei width, GLsizei height,
                              GLenum format, GLenum type, GLintptr offset);
    void texImage3DFromPBO(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
                           GLsizei depth, GLint border, GLenum format, GLenum type, GLintptr offset);
    void texSubImage3DFromPBO(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset,
                              GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type,
                              GLintptr offset);

    // --- Copies (framebuffer -> texture) ---
    void copyTexImage2D(GLenum target, GLint level, GLenum internalformat,
                        GLint x, GLint y, GLsizei width, GLsizei height, GLint border);
    void copyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                           GLint x, GLint y, GLsizei width, GLsizei height);
    void copyTexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset,
                           GLint x, GLint y, GLsizei width, GLsizei height);

    // --- Compressed textures ---
    /// COMPRESSED_TEXTURE_FORMATS: the formats of the enabled compressed
    /// texture extensions, each one the device samples natively.
    std::vector<GLint> compressedTextureFormats() const;
    void compressedTexImage2D(GLenum target, GLint level, GLenum internalformat,
                              GLsizei width, GLsizei height, GLint border,
                              const void* data, size_t dataLen);
    void compressedTexSubImage2D(GLenum target, GLint level,
                                 GLint xoffset, GLint yoffset,
                                 GLsizei width, GLsizei height, GLenum format,
                                 const void* data, size_t dataLen);
    void compressedTexImage3D(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height,
                              GLsizei depth, GLint border, const void* data, size_t dataLen);
    void compressedTexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset,
                                 GLsizei width, GLsizei height, GLsizei depth, GLenum format, const void* data,
                                 size_t dataLen);
    /// The compressed uploads from the bound PIXEL_UNPACK_BUFFER: `size`
    /// bytes at byte `offset`.
    void compressedTexImageFromPBO(GLenum target, GLint level, GLenum internalformat, GLsizei width,
                                   GLsizei height, GLsizei depth, GLint border, GLsizei size, GLintptr offset,
                                   bool is3D);
    void compressedTexSubImageFromPBO(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset,
                                      GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLsizei size,
                                      GLintptr offset, bool is3D);

    // --- VAO ---
    WebGLVertexArrayObject createVertexArray();
    void deleteVertexArray(WebGLVertexArrayObject vao);
    void bindVertexArray(WebGLVertexArrayObject vao);

    // --- Vertex attributes ---
    void vertexAttribPointer(GLuint index, GLint size, GLenum type,
                             GLboolean normalized, GLsizei stride, GLintptr offset);
    void vertexAttribIPointer(GLuint index, GLint size, GLenum type,
                              GLsizei stride, GLintptr offset);
    void enableVertexAttribArray(GLuint index);
    void disableVertexAttribArray(GLuint index);
    void vertexAttribDivisor(GLuint index, GLuint divisor);
    void vertexAttribI4i(GLuint index, GLint x, GLint y, GLint z, GLint w);
    void vertexAttribI4ui(GLuint index, GLuint x, GLuint y, GLuint z, GLuint w);
    void vertexAttribI4iv(GLuint index, const GLint* v);
    void vertexAttribI4uiv(GLuint index, const GLuint* v);
    void vertexAttrib1f(GLuint index, float x);
    void vertexAttrib2f(GLuint index, float x, float y);
    void vertexAttrib3f(GLuint index, float x, float y, float z);
    void vertexAttrib4f(GLuint index, float x, float y, float z, float w);
    void vertexAttrib1fv(GLuint index, const float* v);
    void vertexAttrib2fv(GLuint index, const float* v);
    void vertexAttrib3fv(GLuint index, const float* v);
    void vertexAttrib4fv(GLuint index, const float* v);

    // --- Shaders ---
    WebGLShader createShader(GLenum type);
    void deleteShader(WebGLShader shader);
    void shaderSource(WebGLShader shader, const std::string& source);
    void compileShader(WebGLShader shader);
    GLboolean getShaderParameter_compileStatus(WebGLShader shader);
    std::string getShaderInfoLog(WebGLShader shader);

    // --- Programs ---
    WebGLProgram createProgram();
    void deleteProgram(WebGLProgram program);
    void attachShader(WebGLProgram program, WebGLShader shader);
    void detachShader(WebGLProgram program, WebGLShader shader);
    void linkProgram(WebGLProgram program);
    void useProgram(WebGLProgram program);
    GLboolean getProgramParameter_linkStatus(WebGLProgram program);
    std::string getProgramInfoLog(WebGLProgram program);
    void bindAttribLocation(WebGLProgram program, GLuint index, const std::string& name);
    GLint getAttribLocation(WebGLProgram program, const std::string& name);
    GLint getFragDataLocation(WebGLProgram program, const std::string& name);
    WebGLUniformLocation getUniformLocation(WebGLProgram program, const std::string& name);
    WebGLActiveInfo getActiveAttrib(WebGLProgram program, GLuint index);
    WebGLActiveInfo getActiveUniform(WebGLProgram program, GLuint index);
    GLint getProgramParameter_int(WebGLProgram program, GLenum pname);

    // --- Uniform Block (WebGL2/UBO) ---
    GLuint getUniformBlockIndex(WebGLProgram program, const std::string& name);
    void uniformBlockBinding(WebGLProgram program, GLuint blockIndex, GLuint blockBinding);

    // --- Uniform / block introspection (WebGL2) ---
    std::vector<GLuint> getUniformIndices(WebGLProgram program,
                                          const std::vector<std::string>& names);
    std::vector<GLint> getActiveUniforms(WebGLProgram program,
                                         const std::vector<GLuint>& indices, GLenum pname);
    GLint getActiveUniformBlockParameteri(WebGLProgram program, GLuint blockIndex, GLenum pname);
    std::vector<GLint> getActiveUniformBlockIndices(WebGLProgram program, GLuint blockIndex);
    std::string getActiveUniformBlockName(WebGLProgram program, GLuint blockIndex);

    // --- Uniforms ---
    void uniform1f(WebGLUniformLocation loc, GLfloat x);
    void uniform2f(WebGLUniformLocation loc, GLfloat x, GLfloat y);
    void uniform3f(WebGLUniformLocation loc, GLfloat x, GLfloat y, GLfloat z);
    void uniform4f(WebGLUniformLocation loc, GLfloat x, GLfloat y, GLfloat z, GLfloat w);
    void uniform1i(WebGLUniformLocation loc, GLint x);
    void uniform2i(WebGLUniformLocation loc, GLint x, GLint y);
    void uniform3i(WebGLUniformLocation loc, GLint x, GLint y, GLint z);
    void uniform4i(WebGLUniformLocation loc, GLint x, GLint y, GLint z, GLint w);
    void uniform1fv(WebGLUniformLocation loc, GLsizei count, const GLfloat* value);
    void uniform2fv(WebGLUniformLocation loc, GLsizei count, const GLfloat* value);
    void uniform3fv(WebGLUniformLocation loc, GLsizei count, const GLfloat* value);
    void uniform4fv(WebGLUniformLocation loc, GLsizei count, const GLfloat* value);
    void uniform1ui(WebGLUniformLocation loc, GLuint x);
    void uniform2ui(WebGLUniformLocation loc, GLuint x, GLuint y);
    void uniform3ui(WebGLUniformLocation loc, GLuint x, GLuint y, GLuint z);
    void uniform4ui(WebGLUniformLocation loc, GLuint x, GLuint y, GLuint z, GLuint w);
    void uniform1uiv(WebGLUniformLocation loc, GLsizei count, const GLuint* value);
    void uniform2uiv(WebGLUniformLocation loc, GLsizei count, const GLuint* value);
    void uniform3uiv(WebGLUniformLocation loc, GLsizei count, const GLuint* value);
    void uniform4uiv(WebGLUniformLocation loc, GLsizei count, const GLuint* value);
    void uniform1iv(WebGLUniformLocation loc, GLsizei count, const GLint* value);
    void uniform2iv(WebGLUniformLocation loc, GLsizei count, const GLint* value);
    void uniform3iv(WebGLUniformLocation loc, GLsizei count, const GLint* value);
    void uniform4iv(WebGLUniformLocation loc, GLsizei count, const GLint* value);
    void uniformMatrix2fv(WebGLUniformLocation loc, GLsizei count, GLboolean transpose, const GLfloat* value);
    void uniformMatrix3fv(WebGLUniformLocation loc, GLsizei count, GLboolean transpose, const GLfloat* value);
    void uniformMatrix4fv(WebGLUniformLocation loc, GLsizei count, GLboolean transpose, const GLfloat* value);
    void uniformMatrix2x3fv(WebGLUniformLocation loc, GLsizei count, GLboolean transpose, const GLfloat* value);
    void uniformMatrix3x2fv(WebGLUniformLocation loc, GLsizei count, GLboolean transpose, const GLfloat* value);
    void uniformMatrix2x4fv(WebGLUniformLocation loc, GLsizei count, GLboolean transpose, const GLfloat* value);
    void uniformMatrix4x2fv(WebGLUniformLocation loc, GLsizei count, GLboolean transpose, const GLfloat* value);
    void uniformMatrix3x4fv(WebGLUniformLocation loc, GLsizei count, GLboolean transpose, const GLfloat* value);
    void uniformMatrix4x3fv(WebGLUniformLocation loc, GLsizei count, GLboolean transpose, const GLfloat* value);

    // --- Textures ---
    WebGLTexture createTexture();
    void deleteTexture(WebGLTexture tex);
    void bindTexture(GLenum target, WebGLTexture tex);
    void activeTexture(GLenum texture);
    void texParameteri(GLenum target, GLenum pname, GLint param);
    void texParameterf(GLenum target, GLenum pname, GLfloat param);
    /// getTexParameter: false (with the GL error) for an invalid query.
    bool getTexParameter(GLenum target, GLenum pname, TexParameterValue& out);
    // Uploads carry the client bytes and how many there are (WebGL raises
    // INVALID_OPERATION for too few); null pixels define zeros.
    void texImage2D(GLenum target, GLint level, GLint internalformat,
                    GLsizei width, GLsizei height, GLint border,
                    GLenum format, GLenum type, const void* pixels, size_t size);
    void texSubImage2D(GLenum target, GLint level,
                       GLint xoffset, GLint yoffset,
                       GLsizei width, GLsizei height,
                       GLenum format, GLenum type, const void* pixels, size_t size);
    void texImage3D(GLenum target, GLint level, GLint internalformat,
                    GLsizei width, GLsizei height, GLsizei depth, GLint border,
                    GLenum format, GLenum type, const void* pixels, size_t size);
    void texSubImage3D(GLenum target, GLint level,
                       GLint xoffset, GLint yoffset, GLint zoffset,
                       GLsizei width, GLsizei height, GLsizei depth,
                       GLenum format, GLenum type, const void* pixels, size_t size);
    /// A decoded DOM source (Image, canvas, video, ImageBitmap, ImageData):
    /// tightly packed RGBA8, top row first. width/height < 0 take its size.
    void texImage2DSource(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
                          GLenum format, GLenum type, const uint8_t* rgba, uint32_t srcWidth,
                          uint32_t srcHeight);
    void texSubImage2DSource(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width,
                             GLsizei height, GLenum format, GLenum type, const uint8_t* rgba,
                             uint32_t srcWidth, uint32_t srcHeight);
    void generateMipmap(GLenum target);
    void texStorage2D(GLenum target, GLsizei levels, GLenum internalformat,
                      GLsizei width, GLsizei height);
    void texStorage3D(GLenum target, GLsizei levels, GLenum internalformat,
                      GLsizei width, GLsizei height, GLsizei depth);

    // --- Framebuffers ---
    WebGLFramebuffer createFramebuffer();
    void deleteFramebuffer(WebGLFramebuffer fbo);
    void bindFramebuffer(GLenum target, WebGLFramebuffer fbo);
    void framebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget,
                              WebGLTexture tex, GLint level);
    void framebufferRenderbuffer(GLenum target, GLenum attachment,
                                 GLenum renderbuffertarget, WebGLRenderbuffer rbo);
    void framebufferTextureLayer(GLenum target, GLenum attachment, WebGLTexture tex, GLint level, GLint layer);
    GLenum checkFramebufferStatus(GLenum target);
    /// getFramebufferAttachmentParameter: false (GL error set) when invalid.
    /// FRAMEBUFFER_ATTACHMENT_OBJECT_NAME answers through objectType
    /// (TEXTURE / RENDERBUFFER) and objectName, or isNull; others via value.
    bool getFramebufferAttachmentParameter(GLenum target, GLenum attachment, GLenum pname, GLint& value,
                                           GLenum& objectType, GLuint& objectName, bool& isNull);
    GLint getRenderbufferParameter(GLenum target, GLenum pname);
    /// getInternalformatParameter(RENDERBUFFER, format, SAMPLES).
    std::vector<GLint> supportedSampleCounts(GLenum internalformat);
    void readPixels(GLint x, GLint y, GLsizei width, GLsizei height,
                    GLenum format, GLenum type, void* pixels);
    /// WebGL-level readPixels destination validation: returns false (and
    /// records a synthetic error) if dstLen bytes cannot hold the result.
    bool validateReadPixels(GLsizei width, GLsizei height,
                            GLenum format, GLenum type, size_t dstLen);
    void readBuffer(GLenum src);
    void drawBuffers(GLsizei n, const GLenum* bufs);
    void blitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1,
                         GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1,
                         GLbitfield mask, GLenum filter);

    // --- Renderbuffers ---
    WebGLRenderbuffer createRenderbuffer();
    void deleteRenderbuffer(WebGLRenderbuffer rbo);
    void bindRenderbuffer(GLenum target, WebGLRenderbuffer rbo);
    void renderbufferStorage(GLenum target, GLenum internalformat,
                             GLsizei width, GLsizei height);
    void renderbufferStorageMultisample(GLenum target, GLsizei samples,
                                        GLenum internalformat,
                                        GLsizei width, GLsizei height);

    // --- Draw calls ---
    void drawArrays(GLenum mode, GLint first, GLsizei count);
    void drawElements(GLenum mode, GLsizei count, GLenum type, GLintptr offset);
    void drawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instanceCount);
    void drawElementsInstanced(GLenum mode, GLsizei count, GLenum type,
                               GLintptr offset, GLsizei instanceCount);
    void drawRangeElements(GLenum mode, GLuint start, GLuint end,
                           GLsizei count, GLenum type, GLintptr offset);

    // --- Queries (getParameter, getExtension) ---
    GLint getParameterInt(GLenum pname);
    int64_t getParameterInt64(GLenum pname);
    GLfloat getParameterFloat(GLenum pname);
    GLboolean getParameterBool(GLenum pname);
    void getParameterInt2(GLenum pname, GLint* out);
    void getParameterInt4(GLenum pname, GLint* out);
    void getParameterFloat2(GLenum pname, GLfloat* out);
    void getParameterFloat4(GLenum pname, GLfloat* out);
    void getParameterBool4(GLenum pname, GLboolean* out);
    std::string getParameterString(GLenum pname);
    std::string getShadingLanguageVersion();
    std::vector<std::string> getSupportedExtensions();
    bool getExtension(const std::string& name);

    WebGLProgram currentProgram() const { return {sProgram_}; }
    WebGLFramebuffer currentDrawFramebuffer() const;
    WebGLFramebuffer currentReadFramebuffer() const;
    WebGLRenderbuffer currentRenderbuffer() const;
    WebGLVertexArrayObject currentVertexArray() const { return {sVAO_}; }
    WebGLTexture boundTexture(GLenum target) const;
    WebGLSampler boundSampler(GLuint unit) const;
    GLuint activeTextureUnit() const;

    // --- Object predicates (WebGL is* semantics: false for deleted names,
    //     false before first bind for gen-style objects — matches GL) ---
    GLboolean isBuffer(WebGLBuffer buf);
    GLboolean isTexture(WebGLTexture tex);
    GLboolean isFramebuffer(WebGLFramebuffer fbo);
    GLboolean isRenderbuffer(WebGLRenderbuffer rbo);
    GLboolean isProgram(WebGLProgram program);
    GLboolean isShader(WebGLShader shader);
    GLboolean isVertexArray(WebGLVertexArrayObject vao);

    // --- Misc ---
    void flush();
    void finish();
    void hint(GLenum target, GLenum mode);
    void sampleCoverage(GLfloat value, GLboolean invert);

    // --- Context loss (WEBGL_lose_context) ---
    // A lost context has no backend: every call is a no-op answering its
    // zero value, and getError reports CONTEXT_LOST_WEBGL once. The engine
    // fires the canvas's webglcontextlost / webglcontextrestored events.
    enum class ContextEvent : uint8_t { None, Lost, Restored };
    bool isContextLost() const { return lost_; }
    void loseContext();
    void restoreContext();
    /// The context event the engine owes the canvas, taken once. Taking
    /// Restored is what restores: a fresh backend, every object gone.
    ContextEvent takeContextEvent();
    /// The lost event was not cancelled: restoreContext may not restore.
    void forbidRestore() { restoreAllowed_ = false; }

    /// Nothing to re-apply: the Vulkan backend keeps no state in a shared
    /// device context. Kept for the compositor's call after it draws.
    void restoreState();

private:
    int width_;
    int height_;

    /// Which context's shadow state is live in the shared GL context.
    static WebGL2RenderingContext* current_;

    // Apply UNPACK_FLIP_Y_WEBGL / UNPACK_PREMULTIPLY_ALPHA_WEBGL to client
    // pixel data before upload. Returns the pointer to upload (either the
    // original pixels or tmp.data() with the transform applied).
    const void* applyUnpackTransforms(const void* pixels, GLsizei width, GLsizei height,
                                      GLenum format, GLenum type,
                                      std::vector<uint8_t>& tmp) const;

    std::vector<TeardownCallback> teardownCallbacks_;
    std::unique_ptr<vk::WebGLVkContext> vkCtx_;
    render::VulkanContext* device_ = nullptr;  // what a restore builds the backend on

    bool lost_ = false;
    bool restoreAllowed_ = false;
    bool lostErrorPending_ = false;  // getError's one CONTEXT_LOST_WEBGL
    ContextEvent pendingEvent_ = ContextEvent::None;
    GLuint nextObjectId_ = 1;        // the lost backend's, for the restored one

    inline static render::VulkanContext* defaultVulkanContext_ = nullptr;
};

} // namespace bro::webgl
