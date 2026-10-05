#include "webgl/vulkan/webgl_vk_context.h"
#include "render/vulkan_util.h"
#include "util/log.h"

#include <algorithm>
#include <cstring>

namespace bro::webgl::vk {

WebGLVkContext::WebGLVkContext(int width, int height, render::VulkanContext& context)
    : context_(context), stream_(context), canvas_(context), pipelineCache_(context)
{
    canvas_.init(width, height);
    initVulkanResources();
    canvas_.recordInit(commands());

    // Default viewport and scissor match canvas
    viewport(0, 0, width, height);
    scissor(0, 0, width, height);

    // Default VAO
    vaos_[0] = VkVAOResource{};
    currentVaoId_ = 0;
}

WebGLVkContext::~WebGLVkContext() {
    cleanupVulkanResources();
}

void WebGLVkContext::initVulkanResources() {
    frameEndHook_ = context_.frames().addFrameEndHook([this] { flushCommands(); });

    createPlaceholders();

    for (uint32_t i = 0; i < 16; ++i) {
        float* f = reinterpret_cast<float*>(genericAttribs_[i].data());
        f[0] = 0.0f; f[1] = 0.0f; f[2] = 0.0f; f[3] = 1.0f;
    }
}

void WebGLVkContext::cleanupVulkanResources() {
    VkDevice dev = context_.device();
    if (dev == VK_NULL_HANDLE) return;

    flushCommands();
    if (frameEndHook_ != 0) {
        context_.frames().removeFrameEndHook(frameEndHook_);
        frameEndHook_ = 0;
    }

    for (auto& [id, buf] : buffers_) releaseBuffer(buf);
    buffers_.clear();
    for (auto& [id, tex] : textures_) releaseTexture(tex);
    textures_.clear();
    for (auto& [id, rb] : renderbuffers_) releaseTexture(rb.storage);
    renderbuffers_.clear();
    for (auto& [id, smp] : samplers_) releaseSampler(smp.sampler);
    samplers_.clear();

    // Everything below is either never referenced by a recorded command
    // (shader modules) or shared by all of them, so the context waits for its
    // own work before destroying it.
    context_.queue().wait(stream_.lastTicket());

    if (readback_.buffer != VK_NULL_HANDLE) context_.destroyBuffer(readback_.buffer, readback_.allocId);
    readback_ = Readback{};
    destroyPlaceholders();

    shaders_.clear();
    for (auto& [id, prog] : programs_) releaseProgramExecutable(prog);
    programs_.clear();
    vaos_.clear();
    framebuffers_.clear();

    pipelineCache_.clear();

    canvas_.cleanup();
}

void WebGLVkContext::resize(int width, int height) {
    endRendering();
    canvas_.resize(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
    canvas_.recordInit(commands());
    viewport(0, 0, width, height);
    scissor(0, 0, width, height);
}

void WebGLVkContext::unbindCanvasFBO() {
    flushCommands();
}

bool WebGLVkContext::readCanvasPixels(std::vector<uint8_t>& out) {
    const VkDeviceSize size = static_cast<VkDeviceSize>(canvas_.width()) * canvas_.height() * 4;
    void* mapped = readbackMemory(size);
    if (!mapped) return false;
    VkCommandBuffer cmd = transferCommands();
    if (!canvas_.recordCopy(cmd, readback_.buffer)) return false;
    render::cmdMemoryBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
    if (!waitForCommands()) return false;
    out.assign(static_cast<const uint8_t*>(mapped), static_cast<const uint8_t*>(mapped) + size);
    return true;
}

void WebGLVkContext::flush() {
    flushCommands();
}

void WebGLVkContext::finish() {
    waitForCommands();
}

uint64_t WebGLVkContext::insertFence() {
    flushCommands();
    return stream_.lastTicket();
}

bool WebGLVkContext::isFenceSignaled(uint64_t ticket) const {
    return context_.queue().isComplete(ticket);
}

bool WebGLVkContext::waitFence(uint64_t ticket, uint64_t timeoutNs) {
    return context_.queue().wait(ticket, timeoutNs);
}

GLenum WebGLVkContext::getError() {
    GLenum err = pendingError_;
    pendingError_ = GL_NO_ERROR;
    return err;
}

void WebGLVkContext::setSyntheticError(GLenum err) {
    if (pendingError_ == GL_NO_ERROR) {
        pendingError_ = err;
    }
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static bool isValidCap(GLenum cap) {
    return cap == GL_BLEND || cap == GL_DEPTH_TEST || cap == GL_CULL_FACE ||
           cap == GL_SCISSOR_TEST || cap == GL_STENCIL_TEST || cap == 0x0BD0 /* GL_DITHER */ ||
           cap == 0x8037 /* GL_POLYGON_OFFSET_FILL */ || cap == 0x809E /* GL_SAMPLE_ALPHA_TO_COVERAGE */ ||
           cap == 0x80A0 /* GL_SAMPLE_COVERAGE */ || cap == 0x8C89 /* GL_RASTERIZER_DISCARD */;
}

void WebGLVkContext::viewport(GLint x, GLint y, GLsizei w, GLsizei h) {
    if (w < 0 || h < 0) {
        setSyntheticError(GL_INVALID_VALUE);
        return;
    }
    viewport_.x = static_cast<float>(x);
    viewport_.y = static_cast<float>(y);
    viewport_.width = static_cast<float>(w);
    viewport_.height = static_cast<float>(h);
    viewport_.minDepth = 0.0f;
    viewport_.maxDepth = 1.0f;
}

void WebGLVkContext::scissor(GLint x, GLint y, GLsizei w, GLsizei h) {
    if (w < 0 || h < 0) {
        setSyntheticError(GL_INVALID_VALUE);
        return;
    }
    scissor_.offset.x = x;
    scissor_.offset.y = y;
    scissor_.extent.width = static_cast<uint32_t>(w);
    scissor_.extent.height = static_cast<uint32_t>(h);
}

void WebGLVkContext::clearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
    clearColor_[0] = r; clearColor_[1] = g; clearColor_[2] = b; clearColor_[3] = a;
}

void WebGLVkContext::clearDepth(GLfloat depth) {
    clearDepth_ = depth;
}

void WebGLVkContext::clearStencil(GLint s) {
    clearStencil_ = s;
}

void WebGLVkContext::enable(GLenum cap) {
    if (!isValidCap(cap)) {
        setSyntheticError(GL_INVALID_ENUM);
        return;
    }
    if (cap == GL_BLEND) blendEnabled_ = true;
    else if (cap == GL_DEPTH_TEST) depthTestEnabled_ = true;
    else if (cap == GL_CULL_FACE) cullFaceEnabled_ = true;
    else if (cap == GL_SCISSOR_TEST) scissorTest_ = true;
    else if (cap == GL_STENCIL_TEST) stencilTestEnabled_ = true;
    else if (cap == 0x0BD0 /* GL_DITHER */) ditherEnabled_ = true;
    else if (cap == 0x8037 /* GL_POLYGON_OFFSET_FILL */) polygonOffsetFillEnabled_ = true;
    else if (cap == 0x8C89 /* GL_RASTERIZER_DISCARD */) rasterizerDiscardEnabled_ = true;
    else if (cap == 0x809E /* GL_SAMPLE_ALPHA_TO_COVERAGE */) sampleAlphaToCoverageEnabled_ = true;
    else if (cap == 0x80A0 /* GL_SAMPLE_COVERAGE */) sampleCoverageEnabled_ = true;
}

void WebGLVkContext::disable(GLenum cap) {
    if (!isValidCap(cap)) {
        setSyntheticError(GL_INVALID_ENUM);
        return;
    }
    if (cap == GL_BLEND) blendEnabled_ = false;
    else if (cap == GL_DEPTH_TEST) depthTestEnabled_ = false;
    else if (cap == GL_CULL_FACE) cullFaceEnabled_ = false;
    else if (cap == GL_SCISSOR_TEST) scissorTest_ = false;
    else if (cap == GL_STENCIL_TEST) stencilTestEnabled_ = false;
    else if (cap == 0x0BD0 /* GL_DITHER */) ditherEnabled_ = false;
    else if (cap == 0x8037 /* GL_POLYGON_OFFSET_FILL */) polygonOffsetFillEnabled_ = false;
    else if (cap == 0x8C89 /* GL_RASTERIZER_DISCARD */) rasterizerDiscardEnabled_ = false;
    else if (cap == 0x809E /* GL_SAMPLE_ALPHA_TO_COVERAGE */) sampleAlphaToCoverageEnabled_ = false;
    else if (cap == 0x80A0 /* GL_SAMPLE_COVERAGE */) sampleCoverageEnabled_ = false;
}

GLboolean WebGLVkContext::isEnabled(GLenum cap) {
    if (!isValidCap(cap)) {
        setSyntheticError(GL_INVALID_ENUM);
        return GL_FALSE;
    }
    if (cap == GL_BLEND) return blendEnabled_ ? GL_TRUE : GL_FALSE;
    if (cap == GL_DEPTH_TEST) return depthTestEnabled_ ? GL_TRUE : GL_FALSE;
    if (cap == GL_CULL_FACE) return cullFaceEnabled_ ? GL_TRUE : GL_FALSE;
    if (cap == GL_SCISSOR_TEST) return scissorTest_ ? GL_TRUE : GL_FALSE;
    if (cap == GL_STENCIL_TEST) return stencilTestEnabled_ ? GL_TRUE : GL_FALSE;
    if (cap == 0x0BD0 /* GL_DITHER */) return ditherEnabled_ ? GL_TRUE : GL_FALSE;
    if (cap == 0x8037 /* GL_POLYGON_OFFSET_FILL */) return polygonOffsetFillEnabled_ ? GL_TRUE : GL_FALSE;
    if (cap == 0x8C89 /* GL_RASTERIZER_DISCARD */) return rasterizerDiscardEnabled_ ? GL_TRUE : GL_FALSE;
    if (cap == 0x809E /* GL_SAMPLE_ALPHA_TO_COVERAGE */) return sampleAlphaToCoverageEnabled_ ? GL_TRUE : GL_FALSE;
    if (cap == 0x80A0 /* GL_SAMPLE_COVERAGE */) return sampleCoverageEnabled_ ? GL_TRUE : GL_FALSE;
    return GL_FALSE;
}

void WebGLVkContext::depthFunc(GLenum func) { depthFunc_ = func; }
void WebGLVkContext::depthMask(GLboolean flag) { depthMask_ = flag; }
void WebGLVkContext::depthRange(GLfloat /*zNear*/, GLfloat /*zFar*/) {}
void WebGLVkContext::blendFunc(GLenum sfactor, GLenum dfactor) {
    blendFuncSeparate(sfactor, dfactor, sfactor, dfactor);
}
void WebGLVkContext::blendFuncSeparate(GLenum srcRGB, GLenum dstRGB, GLenum srcAlpha, GLenum dstAlpha) {
    blendSrcRGB_ = srcRGB; blendDstRGB_ = dstRGB; blendSrcAlpha_ = srcAlpha; blendDstAlpha_ = dstAlpha;
}
void WebGLVkContext::blendEquation(GLenum mode) { blendEquationSeparate(mode, mode); }
void WebGLVkContext::blendEquationSeparate(GLenum modeRGB, GLenum modeAlpha) {
    blendEqRGB_ = modeRGB; blendEqAlpha_ = modeAlpha;
}
void WebGLVkContext::blendColor(GLfloat /*r*/, GLfloat /*g*/, GLfloat /*b*/, GLfloat /*a*/) {}
void WebGLVkContext::colorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) {
    colorMask_[0] = r; colorMask_[1] = g; colorMask_[2] = b; colorMask_[3] = a;
}

void WebGLVkContext::stencilFunc(GLenum func, GLint ref, GLuint mask) {
    stencilFuncSeparate(0x0408 /* GL_FRONT_AND_BACK */, func, ref, mask);
}
void WebGLVkContext::stencilFuncSeparate(GLenum face, GLenum func, GLint ref, GLuint mask) {
    if (face == GL_FRONT || face == 0x0408) {
        stencilFuncFront_ = func;
        stencilRefFront_ = ref;
        stencilValueMaskFront_ = mask;
    }
    if (face == GL_BACK || face == 0x0408) {
        stencilFuncBack_ = func;
        stencilRefBack_ = ref;
        stencilValueMaskBack_ = mask;
    }
}
void WebGLVkContext::stencilOp(GLenum fail, GLenum zfail, GLenum zpass) {
    stencilOpSeparate(0x0408 /* GL_FRONT_AND_BACK */, fail, zfail, zpass);
}
void WebGLVkContext::stencilOpSeparate(GLenum face, GLenum fail, GLenum zfail, GLenum zpass) {
    if (face == GL_FRONT || face == 0x0408) {
        stencilFailFront_ = fail;
        stencilPassDepthFailFront_ = zfail;
        stencilPassDepthPassFront_ = zpass;
    }
    if (face == GL_BACK || face == 0x0408) {
        stencilFailBack_ = fail;
        stencilPassDepthFailBack_ = zfail;
        stencilPassDepthPassBack_ = zpass;
    }
}
void WebGLVkContext::stencilMask(GLuint mask) {
    stencilMaskSeparate(0x0408 /* GL_FRONT_AND_BACK */, mask);
}
void WebGLVkContext::stencilMaskSeparate(GLenum face, GLuint mask) {
    if (face == GL_FRONT || face == 0x0408) {
        stencilWriteMaskFront_ = mask;
    }
    if (face == GL_BACK || face == 0x0408) {
        stencilWriteMaskBack_ = mask;
    }
}

void WebGLVkContext::cullFace(GLenum mode) { cullFaceMode_ = mode; }
void WebGLVkContext::frontFace(GLenum mode) { frontFaceMode_ = mode; }
void WebGLVkContext::lineWidth(GLfloat /*width*/) {}
void WebGLVkContext::polygonOffset(GLfloat /*factor*/, GLfloat /*units*/) {}

void WebGLVkContext::pixelStorei(GLenum pname, GLint param) {
    if (pname == GL_UNPACK_ALIGNMENT) {
        if (param == 1 || param == 2 || param == 4 || param == 8) {
            unpackAlignment_ = param;
        } else {
            setSyntheticError(GL_INVALID_VALUE);
        }
    } else if (pname == GL_PACK_ALIGNMENT) {
        if (param == 1 || param == 2 || param == 4 || param == 8) {
            packAlignment_ = param;
        } else {
            setSyntheticError(GL_INVALID_VALUE);
        }
    } else if (pname == 0x9240 /* UNPACK_FLIP_Y_WEBGL */) {
        unpackFlipY_ = (param != 0);
    } else if (pname == 0x9241 /* UNPACK_PREMULTIPLY_ALPHA_WEBGL */) {
        unpackPremultiplyAlpha_ = (param != 0);
    } else if (pname == 0x9243 /* UNPACK_COLORSPACE_CONVERSION_WEBGL */) {
        unpackColorspaceConversion_ = param;
    }
}

// ---------------------------------------------------------------------------
// Vertex Arrays (VAO)
// ---------------------------------------------------------------------------

WebGLVertexArrayObject WebGLVkContext::createVertexArray() {
    GLuint id = nextVaoId_++;
    vaos_[id] = VkVAOResource{};
    return {id};
}

void WebGLVkContext::deleteVertexArray(WebGLVertexArrayObject vao) {
    if (vao.id != 0) {
        vaos_.erase(vao.id);
        if (currentVaoId_ == vao.id) currentVaoId_ = 0;
    }
}

void WebGLVkContext::bindVertexArray(WebGLVertexArrayObject vao) {
    currentVaoId_ = vao.id;
    if (vaos_.find(vao.id) == vaos_.end()) {
        vaos_[vao.id] = VkVAOResource{};
    }
    boundElementArrayBuffer_ = vaos_[currentVaoId_].elementArrayBufferId;
}

void WebGLVkContext::enableVertexAttribArray(GLuint index) {
    if (index < 16) {
        vaos_[currentVaoId_].attributes[index].enabled = true;
    }
}

void WebGLVkContext::disableVertexAttribArray(GLuint index) {
    if (index < 16) {
        vaos_[currentVaoId_].attributes[index].enabled = false;
    }
}

void WebGLVkContext::vertexAttribPointer(GLuint index, GLint size, GLenum type,
                                         GLboolean normalized, GLsizei stride, uintptr_t offset) {
    if (index >= 16) return;
    VkVertexAttribute& attr = vaos_[currentVaoId_].attributes[index];
    attr.size = size;
    attr.type = type;
    attr.normalized = normalized;
    attr.stride = (stride == 0) ? (size * 4) : stride;
    attr.offset = offset;
    attr.bufferId = boundArrayBuffer_;
}

void WebGLVkContext::vertexAttribDivisor(GLuint index, GLuint divisor) {
    if (index < 16) {
        vaos_[currentVaoId_].attributes[index].divisor = divisor;
    }
}

} // namespace bro::webgl::vk

