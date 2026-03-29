#include "webgl/webgl2_context.h"
#include "webgl/glsl_translator.h"
#include "util/log.h"

#include <cstring>

namespace bro::webgl {

// ===========================================================================
// Construction / destruction
// ===========================================================================

WebGL2RenderingContext::WebGL2RenderingContext(int width, int height)
    : width_(width), height_(height) {
    createCanvasFBO();
    LOG_INFO("WebGL2RenderingContext created (%dx%d)", width, height);
}

WebGL2RenderingContext::~WebGL2RenderingContext() {
    // Delete all tracked objects
    for (GLuint id : validBuffers_) glDeleteBuffers(1, &id);
    for (GLuint id : validTextures_) glDeleteTextures(1, &id);
    for (GLuint id : validPrograms_) glDeleteProgram(id);
    for (GLuint id : validShaders_) glDeleteShader(id);
    for (GLuint id : validFramebuffers_) glDeleteFramebuffers(1, &id);
    for (GLuint id : validRenderbuffers_) glDeleteRenderbuffers(1, &id);
    for (GLuint id : validVAOs_) glDeleteVertexArrays(1, &id);
    destroyCanvasFBO();
}

void WebGL2RenderingContext::createCanvasFBO() {
    glGenFramebuffers(1, &canvasFBO_);
    glBindFramebuffer(GL_FRAMEBUFFER, canvasFBO_);

    // Color attachment (RGBA8)
    glGenTextures(1, &colorTex_);
    glBindTexture(GL_TEXTURE_2D, colorTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width_, height_, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTex_, 0);

    // Depth-stencil renderbuffer
    glGenRenderbuffers(1, &depthStencilRBO_);
    glBindRenderbuffer(GL_RENDERBUFFER, depthStencilRBO_);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width_, height_);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                              GL_RENDERBUFFER, depthStencilRBO_);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LOG_ERROR("WebGL canvas FBO incomplete: 0x%x", status);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
}

void WebGL2RenderingContext::destroyCanvasFBO() {
    if (depthStencilRBO_) { glDeleteRenderbuffers(1, &depthStencilRBO_); depthStencilRBO_ = 0; }
    if (colorTex_) { glDeleteTextures(1, &colorTex_); colorTex_ = 0; }
    if (canvasFBO_) { glDeleteFramebuffers(1, &canvasFBO_); canvasFBO_ = 0; }
}

void WebGL2RenderingContext::resize(int width, int height) {
    if (width == width_ && height == height_) return;
    width_ = width;
    height_ = height;
    destroyCanvasFBO();
    createCanvasFBO();
}

void WebGL2RenderingContext::bindCanvasFBO() {
    glBindFramebuffer(GL_FRAMEBUFFER, canvasFBO_);
}

void WebGL2RenderingContext::unbindCanvasFBO() {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// ===========================================================================
// State
// ===========================================================================

void WebGL2RenderingContext::viewport(GLint x, GLint y, GLsizei w, GLsizei h) { glViewport(x, y, w, h); }
void WebGL2RenderingContext::scissor(GLint x, GLint y, GLsizei w, GLsizei h) { glScissor(x, y, w, h); }
void WebGL2RenderingContext::clearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { glClearColor(r, g, b, a); }
void WebGL2RenderingContext::clearDepth(GLfloat depth) { glClearDepth(depth); }
void WebGL2RenderingContext::clearStencil(GLint s) { glClearStencil(s); }
void WebGL2RenderingContext::clear(GLbitfield mask) { glClear(mask); }
void WebGL2RenderingContext::enable(GLenum cap) { glEnable(cap); }
void WebGL2RenderingContext::disable(GLenum cap) { glDisable(cap); }
GLboolean WebGL2RenderingContext::isEnabled(GLenum cap) { return glIsEnabled(cap); }
void WebGL2RenderingContext::depthFunc(GLenum func) { glDepthFunc(func); }
void WebGL2RenderingContext::depthMask(GLboolean flag) { glDepthMask(flag); }
void WebGL2RenderingContext::depthRange(GLfloat zNear, GLfloat zFar) { glDepthRange(zNear, zFar); }
void WebGL2RenderingContext::blendFunc(GLenum s, GLenum d) { glBlendFunc(s, d); }
void WebGL2RenderingContext::blendFuncSeparate(GLenum sr, GLenum dr, GLenum sa, GLenum da) { glBlendFuncSeparate(sr, dr, sa, da); }
void WebGL2RenderingContext::blendEquation(GLenum mode) { glBlendEquation(mode); }
void WebGL2RenderingContext::blendEquationSeparate(GLenum modeRGB, GLenum modeA) { glBlendEquationSeparate(modeRGB, modeA); }
void WebGL2RenderingContext::blendColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { glBlendColor(r, g, b, a); }
void WebGL2RenderingContext::colorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) { glColorMask(r, g, b, a); }
void WebGL2RenderingContext::stencilFunc(GLenum f, GLint r, GLuint m) { glStencilFunc(f, r, m); }
void WebGL2RenderingContext::stencilFuncSeparate(GLenum face, GLenum f, GLint r, GLuint m) { glStencilFuncSeparate(face, f, r, m); }
void WebGL2RenderingContext::stencilOp(GLenum f, GLenum zf, GLenum zp) { glStencilOp(f, zf, zp); }
void WebGL2RenderingContext::stencilOpSeparate(GLenum face, GLenum f, GLenum zf, GLenum zp) { glStencilOpSeparate(face, f, zf, zp); }
void WebGL2RenderingContext::stencilMask(GLuint m) { glStencilMask(m); }
void WebGL2RenderingContext::stencilMaskSeparate(GLenum face, GLuint m) { glStencilMaskSeparate(face, m); }
void WebGL2RenderingContext::cullFace(GLenum mode) { glCullFace(mode); }
void WebGL2RenderingContext::frontFace(GLenum mode) { glFrontFace(mode); }
void WebGL2RenderingContext::polygonOffset(GLfloat factor, GLfloat units) { glPolygonOffset(factor, units); }
void WebGL2RenderingContext::lineWidth(GLfloat width) { glLineWidth(width); }

void WebGL2RenderingContext::pixelStorei(GLenum pname, GLint param) {
    switch (pname) {
        case GL_UNPACK_ALIGNMENT: unpackAlignment_ = param; glPixelStorei(pname, param); break;
        case GL_PACK_ALIGNMENT:   packAlignment_ = param;   glPixelStorei(pname, param); break;
        // WebGL-specific (not real GL enums, handled in JS bindings)
        case 0x9240: unpackFlipY_ = param ? GL_TRUE : GL_FALSE; break;              // UNPACK_FLIP_Y_WEBGL
        case 0x9241: unpackPremultiplyAlpha_ = param ? GL_TRUE : GL_FALSE; break;    // UNPACK_PREMULTIPLY_ALPHA_WEBGL
        default: glPixelStorei(pname, param); break;
    }
}

GLenum WebGL2RenderingContext::getError() { return glGetError(); }

// ===========================================================================
// Buffers
// ===========================================================================

WebGLBuffer WebGL2RenderingContext::createBuffer() {
    GLuint id = 0;
    glGenBuffers(1, &id);
    validBuffers_.insert(id);
    return {id};
}

void WebGL2RenderingContext::deleteBuffer(WebGLBuffer buf) {
    if (buf.id && validBuffers_.erase(buf.id)) {
        glDeleteBuffers(1, &buf.id);
    }
}

void WebGL2RenderingContext::bindBuffer(GLenum target, WebGLBuffer buf) {
    glBindBuffer(target, buf.id);
}

void WebGL2RenderingContext::bufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage) {
    glBufferData(target, size, data, usage);
}

void WebGL2RenderingContext::bufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void* data) {
    glBufferSubData(target, offset, size, data);
}

void WebGL2RenderingContext::copyBufferSubData(GLenum readTarget, GLenum writeTarget,
                                                GLintptr readOffset, GLintptr writeOffset, GLsizeiptr size) {
    glCopyBufferSubData(readTarget, writeTarget, readOffset, writeOffset, size);
}

void WebGL2RenderingContext::getBufferSubData(GLenum target, GLintptr srcByteOffset, void* dstData, GLsizeiptr length) {
    glGetBufferSubData(target, srcByteOffset, length, dstData);
}

void WebGL2RenderingContext::bindBufferBase(GLenum target, GLuint index, WebGLBuffer buf) {
    glBindBufferBase(target, index, buf.id);
}

void WebGL2RenderingContext::bindBufferRange(GLenum target, GLuint index, WebGLBuffer buf,
                                              GLintptr offset, GLsizeiptr size) {
    glBindBufferRange(target, index, buf.id, offset, size);
}

// ===========================================================================
// VAO
// ===========================================================================

WebGLVertexArrayObject WebGL2RenderingContext::createVertexArray() {
    GLuint id = 0;
    glGenVertexArrays(1, &id);
    validVAOs_.insert(id);
    return {id};
}

void WebGL2RenderingContext::deleteVertexArray(WebGLVertexArrayObject vao) {
    if (vao.id && validVAOs_.erase(vao.id)) {
        glDeleteVertexArrays(1, &vao.id);
    }
}

void WebGL2RenderingContext::bindVertexArray(WebGLVertexArrayObject vao) {
    glBindVertexArray(vao.id);
}

// ===========================================================================
// Vertex attributes
// ===========================================================================

void WebGL2RenderingContext::vertexAttribPointer(GLuint index, GLint size, GLenum type,
                                                  GLboolean normalized, GLsizei stride, GLintptr offset) {
    glVertexAttribPointer(index, size, type, normalized, stride, (const void*)offset);
}

void WebGL2RenderingContext::vertexAttribIPointer(GLuint index, GLint size, GLenum type,
                                                   GLsizei stride, GLintptr offset) {
    glVertexAttribIPointer(index, size, type, stride, (const void*)offset);
}

void WebGL2RenderingContext::enableVertexAttribArray(GLuint index) { glEnableVertexAttribArray(index); }
void WebGL2RenderingContext::disableVertexAttribArray(GLuint index) { glDisableVertexAttribArray(index); }
void WebGL2RenderingContext::vertexAttribDivisor(GLuint index, GLuint divisor) { glVertexAttribDivisor(index, divisor); }

// ===========================================================================
// Shaders
// ===========================================================================

WebGLShader WebGL2RenderingContext::createShader(GLenum type) {
    GLuint id = glCreateShader(type);
    validShaders_.insert(id);
    return {id, type};
}

void WebGL2RenderingContext::deleteShader(WebGLShader shader) {
    if (shader.id && validShaders_.erase(shader.id)) {
        glDeleteShader(shader.id);
    }
}

void WebGL2RenderingContext::shaderSource(WebGLShader shader, const std::string& source) {
    // Translate GLSL ES 3.00 → GLSL 3.30
    std::string translated = translateGLSL(source, shader.type);
    const char* src = translated.c_str();
    glShaderSource(shader.id, 1, &src, nullptr);
}

void WebGL2RenderingContext::compileShader(WebGLShader shader) {
    glCompileShader(shader.id);
}

GLboolean WebGL2RenderingContext::getShaderParameter_compileStatus(WebGLShader shader) {
    GLint ok = 0;
    glGetShaderiv(shader.id, GL_COMPILE_STATUS, &ok);
    return ok ? GL_TRUE : GL_FALSE;
}

std::string WebGL2RenderingContext::getShaderInfoLog(WebGLShader shader) {
    GLint len = 0;
    glGetShaderiv(shader.id, GL_INFO_LOG_LENGTH, &len);
    if (len <= 0) return "";
    std::string log(len, '\0');
    glGetShaderInfoLog(shader.id, len, nullptr, log.data());
    // Trim trailing null
    while (!log.empty() && log.back() == '\0') log.pop_back();
    return log;
}

// ===========================================================================
// Programs
// ===========================================================================

WebGLProgram WebGL2RenderingContext::createProgram() {
    GLuint id = glCreateProgram();
    validPrograms_.insert(id);
    return {id};
}

void WebGL2RenderingContext::deleteProgram(WebGLProgram program) {
    if (program.id && validPrograms_.erase(program.id)) {
        glDeleteProgram(program.id);
    }
}

void WebGL2RenderingContext::attachShader(WebGLProgram program, WebGLShader shader) {
    glAttachShader(program.id, shader.id);
}

void WebGL2RenderingContext::detachShader(WebGLProgram program, WebGLShader shader) {
    glDetachShader(program.id, shader.id);
}

void WebGL2RenderingContext::linkProgram(WebGLProgram program) {
    glLinkProgram(program.id);
}

void WebGL2RenderingContext::useProgram(WebGLProgram program) {
    glUseProgram(program.id);
}

GLboolean WebGL2RenderingContext::getProgramParameter_linkStatus(WebGLProgram program) {
    GLint ok = 0;
    glGetProgramiv(program.id, GL_LINK_STATUS, &ok);
    return ok ? GL_TRUE : GL_FALSE;
}

std::string WebGL2RenderingContext::getProgramInfoLog(WebGLProgram program) {
    GLint len = 0;
    glGetProgramiv(program.id, GL_INFO_LOG_LENGTH, &len);
    if (len <= 0) return "";
    std::string log(len, '\0');
    glGetProgramInfoLog(program.id, len, nullptr, log.data());
    while (!log.empty() && log.back() == '\0') log.pop_back();
    return log;
}

void WebGL2RenderingContext::bindAttribLocation(WebGLProgram program, GLuint index, const std::string& name) {
    glBindAttribLocation(program.id, index, name.c_str());
}

GLint WebGL2RenderingContext::getAttribLocation(WebGLProgram program, const std::string& name) {
    return glGetAttribLocation(program.id, name.c_str());
}

WebGLUniformLocation WebGL2RenderingContext::getUniformLocation(WebGLProgram program, const std::string& name) {
    GLint loc = glGetUniformLocation(program.id, name.c_str());
    return {loc, program.id};
}

WebGLActiveInfo WebGL2RenderingContext::getActiveAttrib(WebGLProgram program, GLuint index) {
    char name[256];
    GLsizei len = 0;
    GLint size = 0;
    GLenum type = 0;
    glGetActiveAttrib(program.id, index, sizeof(name), &len, &size, &type, name);
    return {std::string(name, len), type, size};
}

WebGLActiveInfo WebGL2RenderingContext::getActiveUniform(WebGLProgram program, GLuint index) {
    char name[256];
    GLsizei len = 0;
    GLint size = 0;
    GLenum type = 0;
    glGetActiveUniform(program.id, index, sizeof(name), &len, &size, &type, name);
    return {std::string(name, len), type, size};
}

GLint WebGL2RenderingContext::getProgramParameter_int(WebGLProgram program, GLenum pname) {
    GLint val = 0;
    glGetProgramiv(program.id, pname, &val);
    return val;
}

GLuint WebGL2RenderingContext::getUniformBlockIndex(WebGLProgram program, const std::string& name) {
    return glGetUniformBlockIndex(program.id, name.c_str());
}

void WebGL2RenderingContext::uniformBlockBinding(WebGLProgram program, GLuint blockIndex, GLuint blockBinding) {
    glUniformBlockBinding(program.id, blockIndex, blockBinding);
}

// ===========================================================================
// Uniforms
// ===========================================================================

void WebGL2RenderingContext::uniform1f(WebGLUniformLocation loc, GLfloat x) { glUniform1f(loc.location, x); }
void WebGL2RenderingContext::uniform2f(WebGLUniformLocation loc, GLfloat x, GLfloat y) { glUniform2f(loc.location, x, y); }
void WebGL2RenderingContext::uniform3f(WebGLUniformLocation loc, GLfloat x, GLfloat y, GLfloat z) { glUniform3f(loc.location, x, y, z); }
void WebGL2RenderingContext::uniform4f(WebGLUniformLocation loc, GLfloat x, GLfloat y, GLfloat z, GLfloat w) { glUniform4f(loc.location, x, y, z, w); }
void WebGL2RenderingContext::uniform1i(WebGLUniformLocation loc, GLint x) { glUniform1i(loc.location, x); }
void WebGL2RenderingContext::uniform2i(WebGLUniformLocation loc, GLint x, GLint y) { glUniform2i(loc.location, x, y); }
void WebGL2RenderingContext::uniform3i(WebGLUniformLocation loc, GLint x, GLint y, GLint z) { glUniform3i(loc.location, x, y, z); }
void WebGL2RenderingContext::uniform4i(WebGLUniformLocation loc, GLint x, GLint y, GLint z, GLint w) { glUniform4i(loc.location, x, y, z, w); }

void WebGL2RenderingContext::uniform1fv(WebGLUniformLocation loc, GLsizei count, const GLfloat* v) { glUniform1fv(loc.location, count, v); }
void WebGL2RenderingContext::uniform2fv(WebGLUniformLocation loc, GLsizei count, const GLfloat* v) { glUniform2fv(loc.location, count, v); }
void WebGL2RenderingContext::uniform3fv(WebGLUniformLocation loc, GLsizei count, const GLfloat* v) { glUniform3fv(loc.location, count, v); }
void WebGL2RenderingContext::uniform4fv(WebGLUniformLocation loc, GLsizei count, const GLfloat* v) { glUniform4fv(loc.location, count, v); }
void WebGL2RenderingContext::uniform1iv(WebGLUniformLocation loc, GLsizei count, const GLint* v) { glUniform1iv(loc.location, count, v); }
void WebGL2RenderingContext::uniform2iv(WebGLUniformLocation loc, GLsizei count, const GLint* v) { glUniform2iv(loc.location, count, v); }
void WebGL2RenderingContext::uniform3iv(WebGLUniformLocation loc, GLsizei count, const GLint* v) { glUniform3iv(loc.location, count, v); }
void WebGL2RenderingContext::uniform4iv(WebGLUniformLocation loc, GLsizei count, const GLint* v) { glUniform4iv(loc.location, count, v); }

void WebGL2RenderingContext::uniformMatrix2fv(WebGLUniformLocation loc, GLsizei count, GLboolean transpose, const GLfloat* v) { glUniformMatrix2fv(loc.location, count, transpose, v); }
void WebGL2RenderingContext::uniformMatrix3fv(WebGLUniformLocation loc, GLsizei count, GLboolean transpose, const GLfloat* v) { glUniformMatrix3fv(loc.location, count, transpose, v); }
void WebGL2RenderingContext::uniformMatrix4fv(WebGLUniformLocation loc, GLsizei count, GLboolean transpose, const GLfloat* v) { glUniformMatrix4fv(loc.location, count, transpose, v); }
void WebGL2RenderingContext::uniformMatrix2x3fv(WebGLUniformLocation loc, GLsizei count, GLboolean transpose, const GLfloat* v) { glUniformMatrix2x3fv(loc.location, count, transpose, v); }
void WebGL2RenderingContext::uniformMatrix3x2fv(WebGLUniformLocation loc, GLsizei count, GLboolean transpose, const GLfloat* v) { glUniformMatrix3x2fv(loc.location, count, transpose, v); }
void WebGL2RenderingContext::uniformMatrix2x4fv(WebGLUniformLocation loc, GLsizei count, GLboolean transpose, const GLfloat* v) { glUniformMatrix2x4fv(loc.location, count, transpose, v); }
void WebGL2RenderingContext::uniformMatrix4x2fv(WebGLUniformLocation loc, GLsizei count, GLboolean transpose, const GLfloat* v) { glUniformMatrix4x2fv(loc.location, count, transpose, v); }
void WebGL2RenderingContext::uniformMatrix3x4fv(WebGLUniformLocation loc, GLsizei count, GLboolean transpose, const GLfloat* v) { glUniformMatrix3x4fv(loc.location, count, transpose, v); }
void WebGL2RenderingContext::uniformMatrix4x3fv(WebGLUniformLocation loc, GLsizei count, GLboolean transpose, const GLfloat* v) { glUniformMatrix4x3fv(loc.location, count, transpose, v); }

// ===========================================================================
// Textures
// ===========================================================================

WebGLTexture WebGL2RenderingContext::createTexture() {
    GLuint id = 0;
    glGenTextures(1, &id);
    validTextures_.insert(id);
    return {id};
}

void WebGL2RenderingContext::deleteTexture(WebGLTexture tex) {
    if (tex.id && validTextures_.erase(tex.id)) {
        glDeleteTextures(1, &tex.id);
    }
}

void WebGL2RenderingContext::bindTexture(GLenum target, WebGLTexture tex) {
    glBindTexture(target, tex.id);
}

void WebGL2RenderingContext::activeTexture(GLenum texture) {
    glActiveTexture(texture);
}

void WebGL2RenderingContext::texParameteri(GLenum target, GLenum pname, GLint param) {
    glTexParameteri(target, pname, param);
}

void WebGL2RenderingContext::texParameterf(GLenum target, GLenum pname, GLfloat param) {
    glTexParameterf(target, pname, param);
}

// Translate WebGL2 unsized internal formats to GL 3.3 Core sized formats
static GLint translateInternalFormat(GLint internalformat, GLenum type) {
    switch (internalformat) {
        case 0x1908: // GL_RGBA
            switch (type) {
                case GL_UNSIGNED_BYTE: return GL_RGBA8;
                case GL_FLOAT: return GL_RGBA32F;
                case GL_HALF_FLOAT: return GL_RGBA16F;
                default: return GL_RGBA8;
            }
        case 0x1907: // GL_RGB
            switch (type) {
                case GL_UNSIGNED_BYTE: return GL_RGB8;
                case GL_FLOAT: return GL_RGB32F;
                case GL_HALF_FLOAT: return GL_RGB16F;
                default: return GL_RGB8;
            }
        case 0x190A: return GL_RG8;   // GL_LUMINANCE_ALPHA → approximate
        case 0x1909: return GL_R8;    // GL_LUMINANCE → approximate
        case 0x1906: return GL_R8;    // GL_ALPHA → approximate
        case GL_RED: return (type == GL_FLOAT) ? GL_R32F : GL_R8;
        case GL_RG: return (type == GL_FLOAT) ? GL_RG32F : GL_RG8;
        case GL_DEPTH_COMPONENT:
            switch (type) {
                case GL_UNSIGNED_SHORT: return GL_DEPTH_COMPONENT16;
                case GL_UNSIGNED_INT: return GL_DEPTH_COMPONENT24;
                case GL_FLOAT: return GL_DEPTH_COMPONENT32F;
                default: return GL_DEPTH_COMPONENT24;
            }
        case GL_DEPTH_STENCIL: return GL_DEPTH24_STENCIL8;
        default: return internalformat; // Already sized (e.g. GL_RGBA8, GL_R16F)
    }
}

void WebGL2RenderingContext::texImage2D(GLenum target, GLint level, GLint internalformat,
                                         GLsizei width, GLsizei height, GLint border,
                                         GLenum format, GLenum type, const void* pixels) {
    glTexImage2D(target, level, translateInternalFormat(internalformat, type),
                 width, height, border, format, type, pixels);
}

void WebGL2RenderingContext::texSubImage2D(GLenum target, GLint level,
                                            GLint xoffset, GLint yoffset,
                                            GLsizei width, GLsizei height,
                                            GLenum format, GLenum type, const void* pixels) {
    glTexSubImage2D(target, level, xoffset, yoffset, width, height, format, type, pixels);
}

void WebGL2RenderingContext::texImage3D(GLenum target, GLint level, GLint internalformat,
                                         GLsizei width, GLsizei height, GLsizei depth, GLint border,
                                         GLenum format, GLenum type, const void* pixels) {
    glTexImage3D(target, level, internalformat, width, height, depth, border, format, type, pixels);
}

void WebGL2RenderingContext::texSubImage3D(GLenum target, GLint level,
                                            GLint xoffset, GLint yoffset, GLint zoffset,
                                            GLsizei width, GLsizei height, GLsizei depth,
                                            GLenum format, GLenum type, const void* pixels) {
    glTexSubImage3D(target, level, xoffset, yoffset, zoffset, width, height, depth, format, type, pixels);
}

void WebGL2RenderingContext::generateMipmap(GLenum target) { glGenerateMipmap(target); }

void WebGL2RenderingContext::texStorage2D(GLenum target, GLsizei levels, GLenum internalformat,
                                           GLsizei width, GLsizei height) {
    glTexStorage2D(target, levels, internalformat, width, height);
}

void WebGL2RenderingContext::texStorage3D(GLenum target, GLsizei levels, GLenum internalformat,
                                           GLsizei width, GLsizei height, GLsizei depth) {
    glTexStorage3D(target, levels, internalformat, width, height, depth);
}

// ===========================================================================
// Framebuffers
// ===========================================================================

WebGLFramebuffer WebGL2RenderingContext::createFramebuffer() {
    GLuint id = 0;
    glGenFramebuffers(1, &id);
    validFramebuffers_.insert(id);
    return {id};
}

void WebGL2RenderingContext::deleteFramebuffer(WebGLFramebuffer fbo) {
    if (fbo.id && validFramebuffers_.erase(fbo.id)) {
        glDeleteFramebuffers(1, &fbo.id);
    }
}

void WebGL2RenderingContext::bindFramebuffer(GLenum target, WebGLFramebuffer fbo) {
    // WebGL: null framebuffer = our canvas FBO (not the real default 0)
    GLuint id = fbo.id ? fbo.id : canvasFBO_;
    glBindFramebuffer(target, id);
}


void WebGL2RenderingContext::framebufferTexture2D(GLenum target, GLenum attachment,
                                                   GLenum textarget, WebGLTexture tex, GLint level) {
    glFramebufferTexture2D(target, attachment, textarget, tex.id, level);
}

void WebGL2RenderingContext::framebufferRenderbuffer(GLenum target, GLenum attachment,
                                                      GLenum renderbuffertarget, WebGLRenderbuffer rbo) {
    glFramebufferRenderbuffer(target, attachment, renderbuffertarget, rbo.id);
}

GLenum WebGL2RenderingContext::checkFramebufferStatus(GLenum target) {
    return glCheckFramebufferStatus(target);
}

void WebGL2RenderingContext::readPixels(GLint x, GLint y, GLsizei width, GLsizei height,
                                         GLenum format, GLenum type, void* pixels) {
    glReadPixels(x, y, width, height, format, type, pixels);
}

void WebGL2RenderingContext::drawBuffers(GLsizei n, const GLenum* bufs) {
    glDrawBuffers(n, bufs);
}

void WebGL2RenderingContext::blitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1,
                                              GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1,
                                              GLbitfield mask, GLenum filter) {
    glBlitFramebuffer(srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask, filter);
}

// ===========================================================================
// Renderbuffers
// ===========================================================================

WebGLRenderbuffer WebGL2RenderingContext::createRenderbuffer() {
    GLuint id = 0;
    glGenRenderbuffers(1, &id);
    validRenderbuffers_.insert(id);
    return {id};
}

void WebGL2RenderingContext::deleteRenderbuffer(WebGLRenderbuffer rbo) {
    if (rbo.id && validRenderbuffers_.erase(rbo.id)) {
        glDeleteRenderbuffers(1, &rbo.id);
    }
}

void WebGL2RenderingContext::bindRenderbuffer(GLenum target, WebGLRenderbuffer rbo) {
    glBindRenderbuffer(target, rbo.id);
}

void WebGL2RenderingContext::renderbufferStorage(GLenum target, GLenum internalformat,
                                                  GLsizei width, GLsizei height) {
    glRenderbufferStorage(target, internalformat, width, height);
}

void WebGL2RenderingContext::renderbufferStorageMultisample(GLenum target, GLsizei samples,
                                                            GLenum internalformat,
                                                            GLsizei width, GLsizei height) {
    glRenderbufferStorageMultisample(target, samples, internalformat, width, height);
}

// ===========================================================================
// Draw calls
// ===========================================================================

void WebGL2RenderingContext::drawArrays(GLenum mode, GLint first, GLsizei count) {
    glDrawArrays(mode, first, count);
}

void WebGL2RenderingContext::drawElements(GLenum mode, GLsizei count, GLenum type, GLintptr offset) {
    glDrawElements(mode, count, type, (const void*)offset);
}

void WebGL2RenderingContext::drawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instanceCount) {
    glDrawArraysInstanced(mode, first, count, instanceCount);
}

void WebGL2RenderingContext::drawElementsInstanced(GLenum mode, GLsizei count, GLenum type,
                                                    GLintptr offset, GLsizei instanceCount) {
    glDrawElementsInstanced(mode, count, type, (const void*)offset, instanceCount);
}

void WebGL2RenderingContext::drawRangeElements(GLenum mode, GLuint start, GLuint end,
                                                GLsizei count, GLenum type, GLintptr offset) {
    glDrawRangeElements(mode, start, end, count, type, (const void*)offset);
}

// ===========================================================================
// Queries / parameters
// ===========================================================================

GLint WebGL2RenderingContext::getParameterInt(GLenum pname) {
    GLint val = 0;
    glGetIntegerv(pname, &val);
    return val;
}

GLfloat WebGL2RenderingContext::getParameterFloat(GLenum pname) {
    GLfloat val = 0;
    glGetFloatv(pname, &val);
    return val;
}

GLboolean WebGL2RenderingContext::getParameterBool(GLenum pname) {
    GLboolean val = GL_FALSE;
    glGetBooleanv(pname, &val);
    return val;
}

std::string WebGL2RenderingContext::getParameterString(GLenum pname) {
    const GLubyte* str = glGetString(pname);
    return str ? std::string(reinterpret_cast<const char*>(str)) : "";
}

std::string WebGL2RenderingContext::getShadingLanguageVersion() {
    return "WebGL GLSL ES 3.00";
}

std::vector<std::string> WebGL2RenderingContext::getSupportedExtensions() {
    // Return extensions that WebGL2 typically exposes (all are core in GL 3.3)
    return {
        "EXT_color_buffer_float",
        "EXT_float_blend",
        "OES_texture_float_linear",
        "EXT_texture_filter_anisotropic",
        "EXT_blend_minmax",
        "OES_vertex_array_object",
        "OES_element_index_uint",
        "OES_standard_derivatives",
        "OES_fbo_render_mipmap",
        "WEBGL_depth_texture",
        "WEBGL_draw_buffers",
        "EXT_shader_texture_lod",
        "EXT_sRGB",
        "EXT_frag_depth",
        "ANGLE_instanced_arrays",
        "OES_texture_half_float",
        "OES_texture_half_float_linear",
        "WEBGL_lose_context",
        "WEBGL_compressed_texture_s3tc",
    };
}

bool WebGL2RenderingContext::getExtension(const std::string& name) {
    // Desktop GL 3.3 natively supports most WebGL2 extensions
    auto exts = getSupportedExtensions();
    for (auto& ext : exts) {
        if (ext == name) return true;
    }
    return false;
}

// ===========================================================================
// Misc
// ===========================================================================

void WebGL2RenderingContext::flush() { glFlush(); }
void WebGL2RenderingContext::finish() { glFinish(); }
void WebGL2RenderingContext::hint(GLenum target, GLenum mode) { glHint(target, mode); }

} // namespace bro::webgl
