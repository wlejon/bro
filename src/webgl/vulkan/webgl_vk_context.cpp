#include "webgl/vulkan/webgl_vk_context.h"
#include "util/log.h"

#include <algorithm>
#include <cstring>

namespace bro::webgl::vk {

WebGLVkContext::WebGLVkContext(int width, int height, render::VulkanContext& context)
    : context_(context), canvas_(context), pipelineCache_(context)
{
    canvas_.init(width, height);
    initVulkanResources();

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
    VkDevice dev = context_.device();

    // 1. Dynamic rendering dispatch
    pfnCmdBeginRendering_ = reinterpret_cast<PFN_vkCmdBeginRendering>(vkGetDeviceProcAddr(dev, "vkCmdBeginRendering"));
    if (!pfnCmdBeginRendering_) {
        pfnCmdBeginRendering_ = reinterpret_cast<PFN_vkCmdBeginRenderingKHR>(vkGetDeviceProcAddr(dev, "vkCmdBeginRenderingKHR"));
    }
    pfnCmdEndRendering_ = reinterpret_cast<PFN_vkCmdEndRendering>(vkGetDeviceProcAddr(dev, "vkCmdEndRendering"));
    if (!pfnCmdEndRendering_) {
        pfnCmdEndRendering_ = reinterpret_cast<PFN_vkCmdEndRenderingKHR>(vkGetDeviceProcAddr(dev, "vkCmdEndRenderingKHR"));
    }

    // 2. Command pool
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = context_.queueFamilies().graphicsFamily;

    if (vkCreateCommandPool(dev, &poolInfo, nullptr, &commandPool_) != VK_SUCCESS) {
        LOG_ERROR("WebGLVkContext: Failed to create command pool");
    }

    // 3. Descriptor pool
    VkDescriptorPoolSize poolSizes[2]{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[0].descriptorCount = 512;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = 512;

    VkDescriptorPoolCreateInfo descPoolInfo{};
    descPoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    descPoolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    descPoolInfo.maxSets = 128;
    descPoolInfo.poolSizeCount = 2;
    descPoolInfo.pPoolSizes = poolSizes;

    if (vkCreateDescriptorPool(dev, &descPoolInfo, nullptr, &descriptorPool_) != VK_SUCCESS) {
        LOG_ERROR("WebGLVkContext: Failed to create descriptor pool");
    }

    // 4. Descriptor set layout for samplers (0..7) and uniform blocks (8..15)
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    for (uint32_t i = 0; i < 8; ++i) {
        VkDescriptorSetLayoutBinding b{};
        b.binding = i;
        b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b.descriptorCount = 1;
        b.stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS;
        bindings.push_back(b);
    }
    for (uint32_t i = 8; i < 16; ++i) {
        VkDescriptorSetLayoutBinding b{};
        b.binding = i;
        b.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        b.descriptorCount = 1;
        b.stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS;
        bindings.push_back(b);
    }

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();

    if (vkCreateDescriptorSetLayout(dev, &layoutInfo, nullptr, &descriptorSetLayout_) != VK_SUCCESS) {
        LOG_ERROR("WebGLVkContext: Failed to create descriptor set layout");
    }

    // 5. Pipeline layout with Push Constants
    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = 128; // Standard 128-byte push constants

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &descriptorSetLayout_;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;

    if (vkCreatePipelineLayout(dev, &pipelineLayoutInfo, nullptr, &pipelineLayout_) != VK_SUCCESS) {
        LOG_ERROR("WebGLVkContext: Failed to create pipeline layout");
    }

    // 6. Dummy 1x1 fallback texture & sampler
    context_.createImage(1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TILING_OPTIMAL,
                         VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, dummyImage_, dummyMemory_);

    VkImageViewCreateInfo dummyViewInfo{};
    dummyViewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    dummyViewInfo.image = dummyImage_;
    dummyViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    dummyViewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    dummyViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    dummyViewInfo.subresourceRange.levelCount = 1;
    dummyViewInfo.subresourceRange.layerCount = 1;
    vkCreateImageView(dev, &dummyViewInfo, nullptr, &dummyView_);

    VkSamplerCreateInfo dummySampInfo{};
    dummySampInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    dummySampInfo.magFilter = VK_FILTER_LINEAR;
    dummySampInfo.minFilter = VK_FILTER_LINEAR;
    dummySampInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    dummySampInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    dummySampInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(dev, &dummySampInfo, nullptr, &dummySampler_);

    VkCommandBuffer dummyCmd = context_.beginSingleTimeCommands();
    context_.transitionImageLayout(dummyImage_, VK_FORMAT_R8G8B8A8_UNORM,
                                   VK_IMAGE_LAYOUT_UNDEFINED,
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, dummyCmd);
    context_.endSingleTimeCommands(dummyCmd);

    // 7. Generic fallback attribute buffer (16 vec4 attributes = 256 bytes)
    for (uint32_t i = 0; i < 16; ++i) {
        float* f = reinterpret_cast<float*>(genericAttribs_[i].data());
        f[0] = 0.0f; f[1] = 0.0f; f[2] = 0.0f; f[3] = 1.0f;
    }
    context_.createBuffer(sizeof(genericAttribs_),
                          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          fallbackConstantBuffer_, fallbackConstantMemory_);
    void* mapped = nullptr;
    if (fallbackConstantMemory_ != VK_NULL_HANDLE &&
        vkMapMemory(dev, fallbackConstantMemory_, 0, sizeof(genericAttribs_), 0, &mapped) == VK_SUCCESS) {
        std::memcpy(mapped, genericAttribs_.data(), sizeof(genericAttribs_));
        vkUnmapMemory(dev, fallbackConstantMemory_);
    }

    // 8. Dummy UBO buffer (256 bytes)
    context_.createBuffer(256, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          dummyUniformBuffer_, dummyUniformMemory_);
}

void WebGLVkContext::cleanupVulkanResources() {
    VkDevice dev = context_.device();
    if (dev == VK_NULL_HANDLE) return;

    submitAndFlush();

    if (dummyUniformBuffer_ != VK_NULL_HANDLE) {
        vkDestroyBuffer(dev, dummyUniformBuffer_, nullptr);
        dummyUniformBuffer_ = VK_NULL_HANDLE;
    }
    if (dummyUniformMemory_ != VK_NULL_HANDLE) {
        vkFreeMemory(dev, dummyUniformMemory_, nullptr);
        dummyUniformMemory_ = VK_NULL_HANDLE;
    }

    if (fallbackConstantBuffer_ != VK_NULL_HANDLE) {
        vkDestroyBuffer(dev, fallbackConstantBuffer_, nullptr);
        fallbackConstantBuffer_ = VK_NULL_HANDLE;
    }
    if (fallbackConstantMemory_ != VK_NULL_HANDLE) {
        vkFreeMemory(dev, fallbackConstantMemory_, nullptr);
        fallbackConstantMemory_ = VK_NULL_HANDLE;
    }

    if (dummySampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(dev, dummySampler_, nullptr);
        dummySampler_ = VK_NULL_HANDLE;
    }
    if (dummyView_ != VK_NULL_HANDLE) {
        vkDestroyImageView(dev, dummyView_, nullptr);
        dummyView_ = VK_NULL_HANDLE;
    }
    if (dummyImage_ != VK_NULL_HANDLE) {
        vkDestroyImage(dev, dummyImage_, nullptr);
        dummyImage_ = VK_NULL_HANDLE;
    }
    if (dummyMemory_ != VK_NULL_HANDLE) {
        vkFreeMemory(dev, dummyMemory_, nullptr);
        dummyMemory_ = VK_NULL_HANDLE;
    }

    // Destroy buffers
    for (auto& [id, buf] : buffers_) {
        if (buf.allocId != 0) context_.destroyBuffer(buf.buffer, buf.allocId);
        else {
            if (buf.buffer != VK_NULL_HANDLE) vkDestroyBuffer(dev, buf.buffer, nullptr);
            if (buf.memory != VK_NULL_HANDLE) vkFreeMemory(dev, buf.memory, nullptr);
        }
    }
    buffers_.clear();

    // Destroy textures
    for (auto& [id, tex] : textures_) {
        if (tex.sampler != VK_NULL_HANDLE) vkDestroySampler(dev, tex.sampler, nullptr);
        if (tex.view != VK_NULL_HANDLE) vkDestroyImageView(dev, tex.view, nullptr);
        if (tex.allocId != 0) context_.destroyImage(tex.image, tex.allocId);
        else {
            if (tex.image != VK_NULL_HANDLE) vkDestroyImage(dev, tex.image, nullptr);
            if (tex.memory != VK_NULL_HANDLE) vkFreeMemory(dev, tex.memory, nullptr);
        }
    }
    textures_.clear();

    // Destroy samplers
    for (auto& [id, smp] : samplers_) {
        if (smp.sampler != VK_NULL_HANDLE) vkDestroySampler(dev, smp.sampler, nullptr);
    }
    samplers_.clear();

    // Destroy shaders
    for (auto& [id, sh] : shaders_) {
        if (sh.module != VK_NULL_HANDLE) {
            vkDestroyShaderModule(dev, sh.module, nullptr);
        }
    }
    shaders_.clear();

    for (auto& [id, prog] : programs_) {
        if (prog.descriptorSet != VK_NULL_HANDLE && descriptorPool_ != VK_NULL_HANDLE) {
            vkFreeDescriptorSets(dev, descriptorPool_, 1, &prog.descriptorSet);
            prog.descriptorSet = VK_NULL_HANDLE;
        }
        if (prog.vertModule != VK_NULL_HANDLE) {
            vkDestroyShaderModule(dev, prog.vertModule, nullptr);
            prog.vertModule = VK_NULL_HANDLE;
        }
        if (prog.fragModule != VK_NULL_HANDLE) {
            vkDestroyShaderModule(dev, prog.fragModule, nullptr);
            prog.fragModule = VK_NULL_HANDLE;
        }
    }
    programs_.clear();
    vaos_.clear();
    framebuffers_.clear();

    pipelineCache_.clear();

    if (pipelineLayout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(dev, pipelineLayout_, nullptr);
        pipelineLayout_ = VK_NULL_HANDLE;
    }
    if (descriptorSetLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(dev, descriptorSetLayout_, nullptr);
        descriptorSetLayout_ = VK_NULL_HANDLE;
    }
    if (descriptorPool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(dev, descriptorPool_, nullptr);
        descriptorPool_ = VK_NULL_HANDLE;
    }
    if (commandPool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(dev, commandPool_, nullptr);
        commandPool_ = VK_NULL_HANDLE;
    }

    canvas_.cleanup();
}

void WebGLVkContext::resize(int width, int height) {
    submitAndFlush();
    canvas_.resize(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
    viewport(0, 0, width, height);
    scissor(0, 0, width, height);
}

void WebGLVkContext::bindCanvasFBO() {
    currentFboId_ = 0;
}

void WebGLVkContext::unbindCanvasFBO() {
    submitAndFlush();
}

bool WebGLVkContext::readCanvasPixels(std::vector<uint8_t>& out) {
    submitAndFlush();
    return canvas_.readCanvasPixels(out);
}

void WebGLVkContext::flush() {
    submitAndFlush();
}

void WebGLVkContext::finish() {
    submitAndFlush();
    context_.waitIdle();
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

void WebGLVkContext::clearBufferfv(GLenum buffer, GLint /*drawbuffer*/, const GLfloat* values) {
    if (buffer == GL_COLOR && values) {
        clearColor(values[0], values[1], values[2], values[3]);
        clear(GL_COLOR_BUFFER_BIT);
    } else if (buffer == GL_DEPTH && values) {
        clearDepth(values[0]);
        clear(GL_DEPTH_BUFFER_BIT);
    }
}

void WebGLVkContext::clearBufferiv(GLenum /*buffer*/, GLint /*drawbuffer*/, const GLint* /*values*/) {}
void WebGLVkContext::clearBufferuiv(GLenum /*buffer*/, GLint /*drawbuffer*/, const GLuint* /*values*/) {}
void WebGLVkContext::clearBufferfi(GLenum /*buffer*/, GLint /*drawbuffer*/, GLfloat depth, GLint stencil) {
    clearDepth(depth);
    clearStencil(stencil);
    clear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
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

GLint WebGLVkContext::getParameterInt(GLenum pname) {
    switch (pname) {
        case GL_MAX_TEXTURE_SIZE: return 8192;
        case GL_MAX_CUBE_MAP_TEXTURE_SIZE: return 8192;
        case GL_MAX_RENDERBUFFER_SIZE: return 8192;
        case GL_MAX_VERTEX_ATTRIBS: return 16;
        case GL_MAX_VERTEX_UNIFORM_VECTORS: return 256;
        case GL_MAX_FRAGMENT_UNIFORM_VECTORS: return 256;
        case GL_MAX_VARYING_VECTORS: return 16;
        case GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS: return 32;
        case GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS: return 16;
        case GL_MAX_TEXTURE_IMAGE_UNITS: return 16;
        case 0x8824 /* GL_MAX_DRAW_BUFFERS */: return 8;
        case 0x8CDF /* GL_MAX_COLOR_ATTACHMENTS */: return 8;
        case GL_MAX_SAMPLES: return 4;
        case 0x8A2F /* GL_MAX_UNIFORM_BUFFER_BINDINGS */: return 36;
        case GL_MAX_UNIFORM_BLOCK_SIZE: return 65536;
        case 0x8073 /* GL_MAX_3D_TEXTURE_SIZE */: return 2048;
        case 0x88FF /* GL_MAX_ARRAY_TEXTURE_LAYERS */: return 2048;
        case 0x8A34 /* GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT */: return 256;
        case GL_DEPTH_FUNC: return depthFunc_;
        case GL_BLEND_SRC_RGB: return blendSrcRGB_;
        case GL_BLEND_DST_RGB: return blendDstRGB_;
        case GL_BLEND_SRC_ALPHA: return blendSrcAlpha_;
        case GL_BLEND_DST_ALPHA: return blendDstAlpha_;
        case 0x8009 /* GL_BLEND_EQUATION_RGB */: return blendEqRGB_;
        case 0x883D /* GL_BLEND_EQUATION_ALPHA */: return blendEqAlpha_;
        case 0x0B45 /* GL_CULL_FACE_MODE */: return cullFaceMode_;
        case 0x0B46 /* GL_FRONT_FACE */: return frontFaceMode_;
        case 0x84E0 /* GL_ACTIVE_TEXTURE */: return activeTextureUnit_ + GL_TEXTURE0;
        case GL_UNPACK_ALIGNMENT: return unpackAlignment_;
        case GL_PACK_ALIGNMENT: return packAlignment_;
        case GL_STENCIL_WRITEMASK: return stencilWriteMaskFront_;
        case GL_STENCIL_BACK_WRITEMASK: return stencilWriteMaskBack_;
        case GL_STENCIL_BITS: return 8;
        case GL_STENCIL_CLEAR_VALUE: return clearStencil_;
        case GL_STENCIL_FUNC: return stencilFuncFront_;
        case GL_STENCIL_VALUE_MASK: return stencilValueMaskFront_;
        case GL_STENCIL_REF: return stencilRefFront_;
        case GL_STENCIL_FAIL: return stencilFailFront_;
        case GL_STENCIL_PASS_DEPTH_FAIL: return stencilPassDepthFailFront_;
        case GL_STENCIL_PASS_DEPTH_PASS: return stencilPassDepthPassFront_;
        case GL_STENCIL_BACK_FUNC: return stencilFuncBack_;
        case GL_STENCIL_BACK_VALUE_MASK: return stencilValueMaskBack_;
        case GL_STENCIL_BACK_REF: return stencilRefBack_;
        case GL_STENCIL_BACK_FAIL: return stencilFailBack_;
        case GL_STENCIL_BACK_PASS_DEPTH_FAIL: return stencilPassDepthFailBack_;
        case GL_STENCIL_BACK_PASS_DEPTH_PASS: return stencilPassDepthPassBack_;
        case 0x8B9A /* GL_IMPLEMENTATION_COLOR_READ_TYPE */:
        case 0x821B:
            return GL_UNSIGNED_BYTE;
        case 0x8B9B /* GL_IMPLEMENTATION_COLOR_READ_FORMAT */:
        case 0x821A:
            return GL_RGBA;
        default: return 0;
    }
}

GLfloat WebGLVkContext::getParameterFloat(GLenum pname) {
    if (pname == 0x0B73 /* GL_DEPTH_CLEAR_VALUE */) return clearDepth_;
    if (pname == GL_LINE_WIDTH) return 1.0f;
    return 1.0f;
}

GLboolean WebGLVkContext::getParameterBool(GLenum pname) {
    if (pname == GL_BLEND) return blendEnabled_ ? GL_TRUE : GL_FALSE;
    if (pname == GL_DEPTH_TEST) return depthTestEnabled_ ? GL_TRUE : GL_FALSE;
    if (pname == 0x0B72 /* GL_DEPTH_WRITEMASK */) return depthMask_;
    if (pname == GL_CULL_FACE) return cullFaceEnabled_ ? GL_TRUE : GL_FALSE;
    if (pname == GL_SCISSOR_TEST) return scissorTest_ ? GL_TRUE : GL_FALSE;
    if (pname == GL_STENCIL_TEST) return stencilTestEnabled_ ? GL_TRUE : GL_FALSE;
    if (pname == 0x0BD0 /* GL_DITHER */) return ditherEnabled_ ? GL_TRUE : GL_FALSE;
    if (pname == 0x8037 /* GL_POLYGON_OFFSET_FILL */) return polygonOffsetFillEnabled_ ? GL_TRUE : GL_FALSE;
    if (pname == 0x8C89 /* GL_RASTERIZER_DISCARD */) return rasterizerDiscardEnabled_ ? GL_TRUE : GL_FALSE;
    return GL_FALSE;
}

void WebGLVkContext::getParameterInt2(GLenum pname, GLint* out) {
    if (pname == GL_MAX_VIEWPORT_DIMS) {
        out[0] = 8192;
        out[1] = 8192;
    } else {
        out[0] = 0; out[1] = 0;
    }
}

void WebGLVkContext::getParameterInt4(GLenum pname, GLint* out) {
    if (pname == GL_VIEWPORT) {
        out[0] = static_cast<GLint>(viewport_.x);
        out[1] = static_cast<GLint>(viewport_.y);
        out[2] = static_cast<GLint>(viewport_.width);
        out[3] = static_cast<GLint>(viewport_.height);
    } else if (pname == GL_SCISSOR_BOX) {
        out[0] = scissor_.offset.x;
        out[1] = scissor_.offset.y;
        out[2] = scissor_.extent.width;
        out[3] = scissor_.extent.height;
    } else {
        out[0] = 0; out[1] = 0; out[2] = 0; out[3] = 0;
    }
}

void WebGLVkContext::getParameterFloat2(GLenum pname, GLfloat* out) {
    if (pname == 0x846D /* GL_ALIASED_POINT_SIZE_RANGE */ || pname == 0x0B12 /* GL_POINT_SIZE_RANGE */) {
        out[0] = 1.0f;
        out[1] = 64.0f;
    } else if (pname == GL_ALIASED_LINE_WIDTH_RANGE) {
        out[0] = 1.0f;
        out[1] = 1.0f;
    } else if (pname == 0x0B70 /* GL_DEPTH_RANGE */) {
        out[0] = 0.0f;
        out[1] = 1.0f;
    } else {
        out[0] = 0.0f; out[1] = 0.0f;
    }
}

void WebGLVkContext::getParameterFloat4(GLenum pname, GLfloat* out) {
    if (pname == 0x0C22 /* GL_COLOR_CLEAR_VALUE */) {
        out[0] = clearColor_[0];
        out[1] = clearColor_[1];
        out[2] = clearColor_[2];
        out[3] = clearColor_[3];
    } else if (pname == 0x8005 /* GL_BLEND_COLOR */) {
        out[0] = 0.0f; out[1] = 0.0f; out[2] = 0.0f; out[3] = 0.0f;
    } else {
        out[0] = 0.0f; out[1] = 0.0f; out[2] = 0.0f; out[3] = 0.0f;
    }
}

void WebGLVkContext::getParameterBool4(GLenum pname, GLboolean* out) {
    if (pname == 0x0C23 /* GL_COLOR_WRITEMASK */) {
        out[0] = colorMask_[0];
        out[1] = colorMask_[1];
        out[2] = colorMask_[2];
        out[3] = colorMask_[3];
    } else {
        out[0] = GL_TRUE; out[1] = GL_TRUE; out[2] = GL_TRUE; out[3] = GL_TRUE;
    }
}

void WebGLVkContext::updateFallbackConstantBuffer() {
    void* mapped = nullptr;
    if (fallbackConstantMemory_ != VK_NULL_HANDLE &&
        vkMapMemory(context_.device(), fallbackConstantMemory_, 0, sizeof(genericAttribs_), 0, &mapped) == VK_SUCCESS) {
        std::memcpy(mapped, genericAttribs_.data(), sizeof(genericAttribs_));
        vkUnmapMemory(context_.device(), fallbackConstantMemory_);
    }
}

void WebGLVkContext::ensureScratchIndexBufferSize(size_t size) {
    if (scratchIndexSize_ >= size && scratchIndexBuffer_ != VK_NULL_HANDLE) return;
    VkDevice dev = context_.device();
    if (scratchIndexBuffer_ != VK_NULL_HANDLE) {
        vkDestroyBuffer(dev, scratchIndexBuffer_, nullptr);
        scratchIndexBuffer_ = VK_NULL_HANDLE;
    }
    if (scratchIndexMemory_ != VK_NULL_HANDLE) {
        vkFreeMemory(dev, scratchIndexMemory_, nullptr);
        scratchIndexMemory_ = VK_NULL_HANDLE;
    }
    scratchIndexSize_ = std::max(size, static_cast<size_t>(1024));
    context_.createBuffer(scratchIndexSize_, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          scratchIndexBuffer_, scratchIndexMemory_);
}

void WebGLVkContext::uploadScratchIndexBuffer(const void* data, size_t size) {
    ensureScratchIndexBufferSize(size);
    void* mapped = nullptr;
    if (scratchIndexMemory_ != VK_NULL_HANDLE &&
        vkMapMemory(context_.device(), scratchIndexMemory_, 0, size, 0, &mapped) == VK_SUCCESS) {
        std::memcpy(mapped, data, size);
        vkUnmapMemory(context_.device(), scratchIndexMemory_);
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

