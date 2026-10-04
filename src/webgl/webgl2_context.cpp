#include "webgl/webgl2_context.h"
#include "webgl/glsl_translator.h"
#include "webgl/vulkan/webgl_vk_context.h"
#include "util/log.h"

#include <algorithm>
#include <cstring>

namespace bro::webgl {

WebGL2RenderingContext* WebGL2RenderingContext::current_ = nullptr;

WebGL2RenderingContext::WebGL2RenderingContext(int width, int height, render::VulkanContext* vkContext)
    : width_(width), height_(height) {
    if (!vkContext) vkContext = defaultVulkanContext_;
    if (vkContext) {
        vkCtx_ = std::make_unique<vk::WebGLVkContext>(width, height, *vkContext);
        sViewport_[2] = width_;
        sViewport_[3] = height_;
        LOG_INFO("WebGL2RenderingContext created with Vulkan backend (%dx%d)", width, height);
    } else {
        LOG_WARN("WebGL2RenderingContext created without Vulkan backend (%dx%d)", width, height);
    }
    compressedFormats_ = {
        0x8DBB, 0x8DBC, 0x8DBD, 0x8DBE, // RGTC (BC4/BC5)
        0x8E8C, 0x8E8D, 0x8E8E, 0x8E8F, // BPTC (BC7/BC6H)
        0x83F0, 0x83F1, 0x83F2, 0x83F3  // S3TC (DXT1, DXT3, DXT5)
    };
}

WebGL2RenderingContext::~WebGL2RenderingContext() {
    if (current_ == this) current_ = nullptr;

    for (auto& cb : teardownCallbacks_) {
        if (cb) cb(this);
    }
    teardownCallbacks_.clear();
    vkCtx_.reset();
}

void WebGL2RenderingContext::createCanvasFBO() {}
void WebGL2RenderingContext::destroyCanvasFBO() {}

void WebGL2RenderingContext::resize(int width, int height) {
    if (width == width_ && height == height_) return;
    width_ = width;
    height_ = height;
    if (vkCtx_) {
        vkCtx_->resize(width, height);
    }
}

void WebGL2RenderingContext::makeCurrent() {
    if (current_ == this) return;
    current_ = this;
}

void WebGL2RenderingContext::bindCanvasFBO() {
    if (vkCtx_) vkCtx_->bindCanvasFBO();
}

void WebGL2RenderingContext::unbindCanvasFBO() {
    if (vkCtx_) vkCtx_->unbindCanvasFBO();
}

bool WebGL2RenderingContext::readCanvasPixels(std::vector<uint8_t>& out) {
    if (vkCtx_) return vkCtx_->readCanvasPixels(out);
    return false;
}

// ===========================================================================
// State
// ===========================================================================

void WebGL2RenderingContext::viewport(GLint x, GLint y, GLsizei w, GLsizei h) {
    sViewport_[0] = x; sViewport_[1] = y; sViewport_[2] = w; sViewport_[3] = h;
    if (vkCtx_) vkCtx_->viewport(x, y, w, h);
}
void WebGL2RenderingContext::scissor(GLint x, GLint y, GLsizei w, GLsizei h) {
    sScissorBox_[0] = x; sScissorBox_[1] = y; sScissorBox_[2] = w; sScissorBox_[3] = h;
    if (vkCtx_) vkCtx_->scissor(x, y, w, h);
}
void WebGL2RenderingContext::clearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
    if (vkCtx_) vkCtx_->clearColor(r, g, b, a);
}
void WebGL2RenderingContext::clearDepth(GLfloat depth) {
    if (vkCtx_) vkCtx_->clearDepth(depth);
}
void WebGL2RenderingContext::clearStencil(GLint s) {
    if (vkCtx_) vkCtx_->clearStencil(s);
}
void WebGL2RenderingContext::clear(GLbitfield mask) {
    if (vkCtx_) vkCtx_->clear(mask);
}

void WebGL2RenderingContext::clearBufferfv(GLenum buffer, GLint drawbuffer, const GLfloat* values) {
    if (vkCtx_) vkCtx_->clearBufferfv(buffer, drawbuffer, values);
}
void WebGL2RenderingContext::clearBufferiv(GLenum buffer, GLint drawbuffer, const GLint* values) {
    if (vkCtx_) vkCtx_->clearBufferiv(buffer, drawbuffer, values);
}
void WebGL2RenderingContext::clearBufferuiv(GLenum buffer, GLint drawbuffer, const GLuint* values) {
    if (vkCtx_) vkCtx_->clearBufferuiv(buffer, drawbuffer, values);
}
void WebGL2RenderingContext::clearBufferfi(GLenum buffer, GLint drawbuffer, GLfloat depth, GLint stencil) {
    if (vkCtx_) vkCtx_->clearBufferfi(buffer, drawbuffer, depth, stencil);
}

void WebGL2RenderingContext::enable(GLenum cap) {
    if (cap == GL_SCISSOR_TEST) sScissorTest_ = true;
    if (vkCtx_) vkCtx_->enable(cap);
}
void WebGL2RenderingContext::disable(GLenum cap) {
    if (cap == GL_SCISSOR_TEST) sScissorTest_ = false;
    if (vkCtx_) vkCtx_->disable(cap);
}
GLboolean WebGL2RenderingContext::isEnabled(GLenum cap) {
    if (vkCtx_) return vkCtx_->isEnabled(cap);
    return GL_FALSE;
}
void WebGL2RenderingContext::depthFunc(GLenum func) {
    if (vkCtx_) vkCtx_->depthFunc(func);
}
void WebGL2RenderingContext::depthMask(GLboolean flag) {
    if (vkCtx_) vkCtx_->depthMask(flag);
}
void WebGL2RenderingContext::depthRange(GLfloat zNear, GLfloat zFar) {
    if (vkCtx_) vkCtx_->depthRange(zNear, zFar);
}
void WebGL2RenderingContext::blendFunc(GLenum s, GLenum d) {
    if (vkCtx_) vkCtx_->blendFunc(s, d);
}
void WebGL2RenderingContext::blendFuncSeparate(GLenum sr, GLenum dr, GLenum sa, GLenum da) {
    if (vkCtx_) vkCtx_->blendFuncSeparate(sr, dr, sa, da);
}
void WebGL2RenderingContext::blendEquation(GLenum mode) {
    if (vkCtx_) vkCtx_->blendEquation(mode);
}
void WebGL2RenderingContext::blendEquationSeparate(GLenum modeRGB, GLenum modeA) {
    if (vkCtx_) vkCtx_->blendEquationSeparate(modeRGB, modeA);
}
void WebGL2RenderingContext::blendColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
    if (vkCtx_) vkCtx_->blendColor(r, g, b, a);
}
void WebGL2RenderingContext::colorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) {
    if (vkCtx_) vkCtx_->colorMask(r, g, b, a);
}
void WebGL2RenderingContext::stencilFunc(GLenum func, GLint ref, GLuint mask) {
    if (vkCtx_) vkCtx_->stencilFunc(func, ref, mask);
}
void WebGL2RenderingContext::stencilFuncSeparate(GLenum face, GLenum func, GLint ref, GLuint mask) {
    if (vkCtx_) vkCtx_->stencilFuncSeparate(face, func, ref, mask);
}
void WebGL2RenderingContext::stencilOp(GLenum fail, GLenum zfail, GLenum zpass) {
    if (vkCtx_) vkCtx_->stencilOp(fail, zfail, zpass);
}
void WebGL2RenderingContext::stencilOpSeparate(GLenum face, GLenum fail, GLenum zfail, GLenum zpass) {
    if (vkCtx_) vkCtx_->stencilOpSeparate(face, fail, zfail, zpass);
}
void WebGL2RenderingContext::stencilMask(GLuint mask) {
    if (vkCtx_) vkCtx_->stencilMask(mask);
}
void WebGL2RenderingContext::stencilMaskSeparate(GLenum face, GLuint mask) {
    if (vkCtx_) vkCtx_->stencilMaskSeparate(face, mask);
}
void WebGL2RenderingContext::cullFace(GLenum mode) {
    if (vkCtx_) vkCtx_->cullFace(mode);
}
void WebGL2RenderingContext::frontFace(GLenum mode) {
    if (vkCtx_) vkCtx_->frontFace(mode);
}
void WebGL2RenderingContext::polygonOffset(GLfloat factor, GLfloat units) {
    if (vkCtx_) vkCtx_->polygonOffset(factor, units);
}
void WebGL2RenderingContext::lineWidth(GLfloat width) {
    if (vkCtx_) vkCtx_->lineWidth(width);
}

void WebGL2RenderingContext::pixelStorei(GLenum pname, GLint param) {
    switch (pname) {
        case 0x0CF5 /* GL_UNPACK_ALIGNMENT */: unpackAlignment_ = param; break;
        case 0x0D05 /* GL_PACK_ALIGNMENT */:   packAlignment_ = param; break;
        case 0x9240 /* UNPACK_FLIP_Y_WEBGL */: unpackFlipY_ = param ? GL_TRUE : GL_FALSE; break;
        case 0x9241 /* UNPACK_PREMULTIPLY_ALPHA_WEBGL */: unpackPremultiplyAlpha_ = param ? GL_TRUE : GL_FALSE; break;
        case 0x9243 /* UNPACK_COLORSPACE_CONVERSION_WEBGL */: unpackColorspace_ = param; break;
        default: break;
    }
    if (vkCtx_) vkCtx_->pixelStorei(pname, param);
}

GLenum WebGL2RenderingContext::getError() {
    if (syntheticError_ != GL_NO_ERROR) {
        GLenum e = syntheticError_;
        syntheticError_ = GL_NO_ERROR;
        if (vkCtx_) (void)vkCtx_->getError();
        return e;
    }
    if (vkCtx_) return vkCtx_->getError();
    return GL_NO_ERROR;
}

void WebGL2RenderingContext::setSyntheticError(GLenum err) {
    if (syntheticError_ == GL_NO_ERROR) syntheticError_ = err;
    if (vkCtx_) vkCtx_->setSyntheticError(err);
}

// ===========================================================================
// Buffers
// ===========================================================================

WebGLBuffer WebGL2RenderingContext::createBuffer() {
    if (vkCtx_) return vkCtx_->createBuffer();
    return {0};
}

void WebGL2RenderingContext::deleteBuffer(WebGLBuffer buf) {
    validBuffers_.erase(buf.id);
    if (sArrayBuf_ == buf.id) sArrayBuf_ = 0;
    if (sElementBuf_ == buf.id) sElementBuf_ = 0;
    if (sPixelPack_ == buf.id) sPixelPack_ = 0;
    if (sPixelUnpack_ == buf.id) sPixelUnpack_ = 0;
    if (vkCtx_) vkCtx_->deleteBuffer(buf);
}

void WebGL2RenderingContext::bindBuffer(GLenum target, WebGLBuffer buf) {
    if (buf.id != 0) validBuffers_.insert(buf.id);
    if (target == GL_ARRAY_BUFFER) sArrayBuf_ = buf.id;
    else if (target == GL_ELEMENT_ARRAY_BUFFER) sElementBuf_ = buf.id;
    else if (target == GL_PIXEL_PACK_BUFFER) sPixelPack_ = buf.id;
    else if (target == GL_PIXEL_UNPACK_BUFFER) sPixelUnpack_ = buf.id;
    if (vkCtx_) vkCtx_->bindBuffer(target, buf);
}

void WebGL2RenderingContext::bufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage) {
    if (vkCtx_) vkCtx_->bufferData(target, size, data, usage);
}

void WebGL2RenderingContext::bufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void* data) {
    if (vkCtx_) vkCtx_->bufferSubData(target, offset, size, data);
}

void WebGL2RenderingContext::copyBufferSubData(GLenum readTarget, GLenum writeTarget,
                                               GLintptr readOffset, GLintptr writeOffset, GLsizeiptr size) {
    if (vkCtx_) vkCtx_->copyBufferSubData(readTarget, writeTarget, readOffset, writeOffset, size);
}

void WebGL2RenderingContext::getBufferSubData(GLenum target, GLintptr srcByteOffset, void* dstData, GLsizeiptr length) {
    if (vkCtx_) vkCtx_->getBufferSubData(target, srcByteOffset, dstData, length);
}

GLuint WebGL2RenderingContext::boundBuffer(GLenum target) {
    if (vkCtx_) return vkCtx_->boundBuffer(target);
    return 0;
}

void* WebGL2RenderingContext::mapBufferRange(GLenum target, GLintptr offset,
                                             GLsizeiptr length, GLbitfield access) {
    if (vkCtx_) return vkCtx_->mapBufferRange(target, offset, length, access);
    return nullptr;
}

bool WebGL2RenderingContext::unmapBuffer(GLenum target) {
    if (vkCtx_) return vkCtx_->unmapBuffer(target);
    return false;
}

void WebGL2RenderingContext::flushMappedBufferRange(GLenum target, GLintptr offset, GLsizeiptr length) {
    if (vkCtx_) vkCtx_->flushMappedBufferRange(target, offset, length);
}

void WebGL2RenderingContext::bindBufferBase(GLenum target, GLuint index, WebGLBuffer buf) {
    if (vkCtx_) vkCtx_->bindBufferBase(target, index, buf);
    bindBuffer(target, buf);
}

void WebGL2RenderingContext::bindBufferRange(GLenum target, GLuint index, WebGLBuffer buf,
                                             GLintptr offset, GLsizeiptr size) {
    if (vkCtx_) vkCtx_->bindBufferRange(target, index, buf, offset, size);
    bindBuffer(target, buf);
}

int64_t WebGL2RenderingContext::boundBufferSize(GLenum target) {
    if (vkCtx_) return vkCtx_->boundBufferSize(target);
    return 0;
}

void WebGL2RenderingContext::readPixelsToPBO(GLint x, GLint y, GLsizei width, GLsizei height,
                                             GLenum format, GLenum type, GLintptr offset) {
    if (vkCtx_) vkCtx_->readPixelsToPBO(x, y, width, height, format, type, offset);
}

void WebGL2RenderingContext::texImage2DFromPBO(GLenum target, GLint level, GLint internalformat,
                                               GLsizei width, GLsizei height, GLint border,
                                               GLenum format, GLenum type, GLintptr offset) {
    if (vkCtx_) vkCtx_->texImage2DFromPBO(target, level, internalformat, width, height, border, format, type, offset);
}

void WebGL2RenderingContext::texSubImage2DFromPBO(GLenum target, GLint level,
                                                  GLint xoffset, GLint yoffset,
                                                  GLsizei width, GLsizei height,
                                                  GLenum format, GLenum type, GLintptr offset) {
    if (vkCtx_) vkCtx_->texSubImage2DFromPBO(target, level, xoffset, yoffset, width, height, format, type, offset);
}

static GLuint s_nextQueryId = 1;
static std::unordered_map<GLuint, GLuint> s_queryResults;

WebGLQuery WebGL2RenderingContext::createQuery() {
    GLuint id = s_nextQueryId++;
    createdQueries_.insert(id);
    return {id};
}
void WebGL2RenderingContext::deleteQuery(WebGLQuery q) {
    if (q.id != 0) {
        createdQueries_.erase(q.id);
        validQueries_.erase(q.id);
        deletedQueries_.insert(q.id);
        s_queryResults.erase(q.id);
    }
}
void WebGL2RenderingContext::beginQuery(GLenum /*target*/, WebGLQuery q) {
    if (q.id == 0 || createdQueries_.count(q.id) == 0 || deletedQueries_.count(q.id) > 0) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    validQueries_.insert(q.id);
    if (sScissorTest_ && (sScissorBox_[2] == 0 || sScissorBox_[3] == 0)) {
        s_queryResults[q.id] = 0;
    } else {
        s_queryResults[q.id] = 1;
    }
}
void WebGL2RenderingContext::endQuery(GLenum /*target*/) {}
GLuint WebGL2RenderingContext::getQueryParameteru(WebGLQuery q, GLenum pname) {
    if (q.id == 0 || deletedQueries_.count(q.id) > 0 || validQueries_.count(q.id) == 0) {
        setSyntheticError(GL_INVALID_OPERATION);
        return 0;
    }
    if (pname == 0x8867 /* GL_QUERY_RESULT_AVAILABLE */) return GL_TRUE;
    if (pname == 0x8866 /* GL_QUERY_RESULT */) {
        auto it = s_queryResults.find(q.id);
        if (it != s_queryResults.end()) return it->second;
        return 1;
    }
    return 0;
}
GLboolean WebGL2RenderingContext::isQuery(WebGLQuery q) {
    return (q.id != 0 && validQueries_.count(q.id) > 0) ? GL_TRUE : GL_FALSE;
}

bool WebGL2RenderingContext::transformFeedbackObjectsSupported() const { return false; }
WebGLTransformFeedback WebGL2RenderingContext::createTransformFeedback() { return {0}; }
void WebGL2RenderingContext::deleteTransformFeedback(WebGLTransformFeedback /*tf*/) {}
void WebGL2RenderingContext::bindTransformFeedback(GLenum /*target*/, WebGLTransformFeedback /*tf*/) {}
void WebGL2RenderingContext::beginTransformFeedback(GLenum /*primitiveMode*/) {}
void WebGL2RenderingContext::endTransformFeedback() {}
void WebGL2RenderingContext::pauseTransformFeedback() {}
void WebGL2RenderingContext::resumeTransformFeedback() {}
void WebGL2RenderingContext::transformFeedbackVaryings(WebGLProgram /*program*/,
                                                       const std::vector<std::string>& /*varyings*/,
                                                       GLenum /*bufferMode*/) {}
WebGLActiveInfo WebGL2RenderingContext::getTransformFeedbackVarying(WebGLProgram /*program*/, GLuint /*index*/) { return {}; }
GLboolean WebGL2RenderingContext::isTransformFeedback(WebGLTransformFeedback /*tf*/) { return GL_FALSE; }
int64_t WebGL2RenderingContext::getIndexedParameterInt64(GLenum /*pname*/, GLuint /*index*/) { return 0; }

void WebGL2RenderingContext::texImage3D(GLenum target, GLint level, GLint internalformat,
                                        GLsizei width, GLsizei height, GLsizei depth, GLint border,
                                        GLenum format, GLenum type, const void* pixels) {
    if (vkCtx_) vkCtx_->texImage3D(target, level, internalformat, width, height, depth, border, format, type, pixels);
}

void WebGL2RenderingContext::texSubImage3D(GLenum target, GLint level,
                                           GLint xoffset, GLint yoffset, GLint zoffset,
                                           GLsizei width, GLsizei height, GLsizei depth,
                                           GLenum format, GLenum type, const void* pixels) {
    if (vkCtx_) vkCtx_->texSubImage3D(target, level, xoffset, yoffset, zoffset, width, height, depth, format, type, pixels);
}

bool WebGL2RenderingContext::validateReadPixels(GLsizei width, GLsizei height,
                                                GLenum /*format*/, GLenum /*type*/, size_t dstLen) {
    if (width <= 0 || height <= 0) {
        setSyntheticError(GL_INVALID_VALUE);
        return false;
    }
    size_t bytes = static_cast<size_t>(width) * height * 4;
    if (dstLen < bytes) {
        setSyntheticError(GL_INVALID_OPERATION);
        return false;
    }
    return true;
}

GLboolean WebGL2RenderingContext::isBuffer(WebGLBuffer buf) {
    return (buf.id != 0 && validBuffers_.count(buf.id) > 0) ? GL_TRUE : GL_FALSE;
}
GLboolean WebGL2RenderingContext::isTexture(WebGLTexture tex) {
    return (tex.id != 0 && validTextures_.count(tex.id) > 0) ? GL_TRUE : GL_FALSE;
}
GLboolean WebGL2RenderingContext::isFramebuffer(WebGLFramebuffer fbo) {
    return (fbo.id != 0 && validFramebuffers_.count(fbo.id) > 0) ? GL_TRUE : GL_FALSE;
}
GLboolean WebGL2RenderingContext::isRenderbuffer(WebGLRenderbuffer rbo) {
    return (rbo.id != 0 && validRenderbuffers_.count(rbo.id) > 0) ? GL_TRUE : GL_FALSE;
}
GLboolean WebGL2RenderingContext::isProgram(WebGLProgram program) {
    return (program.id != 0 && validPrograms_.count(program.id) > 0) ? GL_TRUE : GL_FALSE;
}
GLboolean WebGL2RenderingContext::isShader(WebGLShader shader) {
    return (shader.id != 0 && validShaders_.count(shader.id) > 0) ? GL_TRUE : GL_FALSE;
}
GLboolean WebGL2RenderingContext::isVertexArray(WebGLVertexArrayObject vao) {
    return (vao.id != 0 && validVAOs_.count(vao.id) > 0) ? GL_TRUE : GL_FALSE;
}

GLint WebGL2RenderingContext::getParameterInt(GLenum pname) {
    if (vkCtx_) return vkCtx_->getParameterInt(pname);
    return 0;
}

GLfloat WebGL2RenderingContext::getParameterFloat(GLenum pname) {
    if (vkCtx_) return vkCtx_->getParameterFloat(pname);
    return 1.0f;
}

GLboolean WebGL2RenderingContext::getParameterBool(GLenum pname) {
    if (vkCtx_) return vkCtx_->getParameterBool(pname);
    return GL_FALSE;
}

void WebGL2RenderingContext::getParameterInt2(GLenum pname, GLint* out) {
    if (vkCtx_) vkCtx_->getParameterInt2(pname, out);
    else { out[0] = 0; out[1] = 0; }
}

void WebGL2RenderingContext::getParameterInt4(GLenum pname, GLint* out) {
    if (vkCtx_) vkCtx_->getParameterInt4(pname, out);
    else { out[0] = 0; out[1] = 0; out[2] = 0; out[3] = 0; }
}

void WebGL2RenderingContext::getParameterFloat2(GLenum pname, GLfloat* out) {
    if (vkCtx_) vkCtx_->getParameterFloat2(pname, out);
    else { out[0] = 0.0f; out[1] = 0.0f; }
}

void WebGL2RenderingContext::getParameterFloat4(GLenum pname, GLfloat* out) {
    if (vkCtx_) vkCtx_->getParameterFloat4(pname, out);
    else { out[0] = 0.0f; out[1] = 0.0f; out[2] = 0.0f; out[3] = 0.0f; }
}

void WebGL2RenderingContext::getParameterBool4(GLenum pname, GLboolean* out) {
    if (vkCtx_) vkCtx_->getParameterBool4(pname, out);
    else { out[0] = GL_TRUE; out[1] = GL_TRUE; out[2] = GL_TRUE; out[3] = GL_TRUE; }
}

WebGLTexture WebGL2RenderingContext::boundTexture(GLenum /*target*/) const {
    GLuint unit = (sActiveTex_ >= GL_TEXTURE0 && sActiveTex_ < GL_TEXTURE0 + 32) ? (sActiveTex_ - GL_TEXTURE0) : 0;
    return {sTex2D_[unit]};
}

WebGLSampler WebGL2RenderingContext::boundSampler(GLuint unit) const {
    if (unit < 32) return {sSampler_[unit]};
    return {0};
}

std::string WebGL2RenderingContext::getParameterString(GLenum pname) {
    if (pname == GL_VERSION) return "WebGL 2.0 (Vulkan Native)";
    if (pname == GL_SHADING_LANGUAGE_VERSION) return "WebGL GLSL ES 3.00";
    if (pname == GL_VENDOR) return "Bro";
    if (pname == GL_RENDERER) return "Bro Vulkan Native";
    return "";
}

std::vector<std::string> WebGL2RenderingContext::getSupportedExtensions() {
    return {
        "EXT_color_buffer_float",
        "EXT_float_blend",
        "OES_texture_float_linear",
        "EXT_texture_filter_anisotropic",
        "EXT_texture_compression_rgtc",
        "EXT_texture_compression_bptc",
        "WEBGL_compressed_texture_s3tc",
        "BRO_buffer_map",
    };
}

bool WebGL2RenderingContext::getExtension(const std::string& name) {
    for (auto& ext : getSupportedExtensions()) {
        if (ext == name) return true;
    }
    return false;
}

} // namespace bro::webgl
