#pragma once

#include "webgl/webgl_objects.h"
#include <glad/gl.h>

#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <array>
#include <cstdint>

namespace bro::webgl {

/// WebGL2RenderingContext — maps WebGL2 API calls to raw OpenGL 3.3.
///
/// Owns a dedicated FBO that serves as the WebGL canvas. The rendered result
/// (color texture) is composited into the window by WebGLScene.
class WebGL2RenderingContext {
public:
    WebGL2RenderingContext(int width, int height);
    ~WebGL2RenderingContext();

    WebGL2RenderingContext(const WebGL2RenderingContext&) = delete;
    WebGL2RenderingContext& operator=(const WebGL2RenderingContext&) = delete;

    /// Resize the canvas FBO.
    void resize(int width, int height);

    /// Get the color texture of the canvas FBO (for compositing).
    GLuint colorTexture() const { return colorTex_; }
    int canvasWidth() const { return width_; }
    int canvasHeight() const { return height_; }

    /// Bind the canvas FBO as the render target.
    /// Call before issuing WebGL draw commands in the frame loop.
    void bindCanvasFBO();

    /// Unbind the canvas FBO (restore default framebuffer).
    void unbindCanvasFBO();

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
    GLuint getQueryParameteru(WebGLQuery q, GLenum pname);
    GLboolean isQuery(WebGLQuery q);

    // --- Transform feedback (WebGL2) ---
    /// TF *objects* are ARB_transform_feedback2 (core in GL 4.0, an extension
    /// on 3.3 hardware — universal on desktop drivers). Everything else
    /// (begin/end/varyings, the default TF object) is core GL 3.0.
    bool transformFeedbackObjectsSupported() const;
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
    WebGLActiveInfo getTransformFeedbackVarying(WebGLProgram program, GLuint index);
    GLboolean isTransformFeedback(WebGLTransformFeedback tf);
    bool transformFeedbackActive() const { return tfActive_; }
    bool transformFeedbackPaused() const { return tfPaused_; }

    /// getIndexedParameter numeric rows (indexed buffer-range queries).
    int64_t getIndexedParameterInt64(GLenum pname, GLuint index);

    // --- Pixel buffer objects (WebGL2) ---
    GLuint pixelPackBuffer() const { return sPixelPack_; }
    GLuint pixelUnpackBuffer() const { return sPixelUnpack_; }
    /// Byte size of the buffer bound at `target` (0 when none bound).
    int64_t boundBufferSize(GLenum target);
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

    // --- Copies (framebuffer -> texture) ---
    void copyTexImage2D(GLenum target, GLint level, GLenum internalformat,
                        GLint x, GLint y, GLsizei width, GLsizei height, GLint border);
    void copyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                           GLint x, GLint y, GLsizei width, GLsizei height);

    // --- Compressed textures ---
    /// Formats exposed via the driver's real extension support (probed at
    /// context creation). Desktop GL 3.3 has no ETC2 — S3TC/RGTC/BPTC only.
    const std::vector<GLint>& compressedTextureFormats() const { return compressedFormats_; }
    bool isCompressedFormatSupported(GLenum format) const;
    /// Both validate block-size math against dataLen before touching the
    /// driver (the no-overread guarantee for client memory).
    void compressedTexImage2D(GLenum target, GLint level, GLenum internalformat,
                              GLsizei width, GLsizei height, GLint border,
                              const void* data, size_t dataLen);
    void compressedTexSubImage2D(GLenum target, GLint level,
                                 GLint xoffset, GLint yoffset,
                                 GLsizei width, GLsizei height, GLenum format,
                                 const void* data, size_t dataLen);

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
    void texImage2D(GLenum target, GLint level, GLint internalformat,
                    GLsizei width, GLsizei height, GLint border,
                    GLenum format, GLenum type, const void* pixels);
    void texSubImage2D(GLenum target, GLint level,
                       GLint xoffset, GLint yoffset,
                       GLsizei width, GLsizei height,
                       GLenum format, GLenum type, const void* pixels);
    void texImage3D(GLenum target, GLint level, GLint internalformat,
                    GLsizei width, GLsizei height, GLsizei depth, GLint border,
                    GLenum format, GLenum type, const void* pixels);
    void texSubImage3D(GLenum target, GLint level,
                       GLint xoffset, GLint yoffset, GLint zoffset,
                       GLsizei width, GLsizei height, GLsizei depth,
                       GLenum format, GLenum type, const void* pixels);
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
    GLenum checkFramebufferStatus(GLenum target);
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
    GLfloat getParameterFloat(GLenum pname);
    GLboolean getParameterBool(GLenum pname);
    std::string getParameterString(GLenum pname);
    std::string getShadingLanguageVersion();
    std::vector<std::string> getSupportedExtensions();
    bool getExtension(const std::string& name);

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

private:
    void createCanvasFBO();
    void destroyCanvasFBO();

    int width_;
    int height_;

    // Canvas FBO (the WebGL "default framebuffer")
    GLuint canvasFBO_ = 0;
    GLuint colorTex_ = 0;
    GLuint depthStencilRBO_ = 0;

    // Object tracking
    std::unordered_set<GLuint> validBuffers_;
    std::unordered_set<GLuint> validTextures_;
    std::unordered_set<GLuint> validPrograms_;
    std::unordered_set<GLuint> validShaders_;
    std::unordered_set<GLuint> validFramebuffers_;
    std::unordered_set<GLuint> validRenderbuffers_;
    std::unordered_set<GLuint> validVAOs_;
    std::unordered_set<GLuint> validSamplers_;
    std::unordered_set<GLuint> validQueries_;
    std::unordered_set<GLsync> validSyncs_;
    std::unordered_set<GLuint> validTransformFeedbacks_;

    // Transform feedback state (also drives the compositing handoff: an
    // active TF is paused around engine GL work and resumed afterwards).
    bool tfActive_ = false;
    bool tfPaused_ = false;

    // Compressed formats the driver actually supports (probed once).
    std::vector<GLint> compressedFormats_;

    // pixelStorei state
    GLint unpackAlignment_ = 4;
    GLint packAlignment_ = 4;
    GLboolean unpackFlipY_ = GL_FALSE;
    GLboolean unpackPremultiplyAlpha_ = GL_FALSE;
    GLint unpackColorspace_ = 0x9244; // BROWSER_DEFAULT_WEBGL

    // First pending WebGL-level error (returned by getError before real GL errors)
    GLenum syntheticError_ = 0; // GL_NO_ERROR

    // Apply UNPACK_FLIP_Y_WEBGL / UNPACK_PREMULTIPLY_ALPHA_WEBGL to client
    // pixel data before upload. Returns the pointer to upload (either the
    // original pixels or tmp.data() with the transform applied).
    const void* applyUnpackTransforms(const void* pixels, GLsizei width, GLsizei height,
                                      GLenum format, GLenum type,
                                      std::vector<uint8_t>& tmp) const;

    // --- Shadow state for cheap save/restore around compositing ---
public:
    /// Re-apply all shadow-tracked GL state (call after compositing).
    void restoreState();

private:
    // Tracked by our wrapper methods — no glGet* queries needed
    GLfloat sClearR_ = 0, sClearG_ = 0, sClearB_ = 0, sClearA_ = 0;
    GLint sViewport_[4] = {0, 0, 0, 0};
    GLuint sProgram_ = 0;
    GLuint sVAO_ = 0;
    GLuint sArrayBuf_ = 0;
    GLuint sElementBuf_ = 0;
    GLuint sPixelPack_ = 0;
    GLuint sPixelUnpack_ = 0;
    GLenum sActiveTex_ = GL_TEXTURE0;
    GLuint sTex2D_[32] = {};      // per texture unit
    GLuint sSampler_[32] = {};    // per texture unit (sampler objects)
    GLuint sFBO_ = 0;             // stores the raw GL id (canvasFBO_ for null)
    GLint sBlendSrcRGB_ = GL_ONE, sBlendDstRGB_ = GL_ZERO;
    GLint sBlendSrcA_ = GL_ONE, sBlendDstA_ = GL_ZERO;
    GLenum sBlendEqRGB_ = GL_FUNC_ADD, sBlendEqA_ = GL_FUNC_ADD;
    GLboolean sDepthMask_ = GL_TRUE;
    GLboolean sColorMask_[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
    GLenum sDepthFunc_ = GL_LESS;
    GLenum sCullMode_ = GL_BACK;
    GLenum sFrontFace_ = GL_CCW;
    GLuint sTransformFeedback_ = 0;  // bound TF object (0 = default)
    // Capability flags
    bool sBlend_ = false;
    bool sDepthTest_ = false;
    bool sCullFace_ = false;
    bool sScissorTest_ = false;
    bool sStencilTest_ = false;
    bool sRasterizerDiscard_ = false;
};

} // namespace bro::webgl
