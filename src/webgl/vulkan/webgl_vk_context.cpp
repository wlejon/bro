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
    poolSizes[0].descriptorCount = 64;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = 128;

    VkDescriptorPoolCreateInfo descPoolInfo{};
    descPoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    descPoolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    descPoolInfo.maxSets = 64;
    descPoolInfo.poolSizeCount = 2;
    descPoolInfo.pPoolSizes = poolSizes;

    if (vkCreateDescriptorPool(dev, &descPoolInfo, nullptr, &descriptorPool_) != VK_SUCCESS) {
        LOG_ERROR("WebGLVkContext: Failed to create descriptor pool");
    }

    // 4. Descriptor set layout for samplers
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    for (uint32_t i = 0; i < 8; ++i) {
        VkDescriptorSetLayoutBinding b{};
        b.binding = i;
        b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b.descriptorCount = 1;
        b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
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
}

void WebGLVkContext::cleanupVulkanResources() {
    VkDevice dev = context_.device();
    if (dev == VK_NULL_HANDLE) return;

    submitAndFlush();

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
        if (buf.buffer != VK_NULL_HANDLE) vkDestroyBuffer(dev, buf.buffer, nullptr);
        if (buf.memory != VK_NULL_HANDLE) vkFreeMemory(dev, buf.memory, nullptr);
    }
    buffers_.clear();

    // Destroy textures
    for (auto& [id, tex] : textures_) {
        if (tex.sampler != VK_NULL_HANDLE) vkDestroySampler(dev, tex.sampler, nullptr);
        if (tex.view != VK_NULL_HANDLE) vkDestroyImageView(dev, tex.view, nullptr);
        if (tex.image != VK_NULL_HANDLE) vkDestroyImage(dev, tex.image, nullptr);
        if (tex.memory != VK_NULL_HANDLE) vkFreeMemory(dev, tex.memory, nullptr);
    }
    textures_.clear();

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

void WebGLVkContext::viewport(GLint x, GLint y, GLsizei w, GLsizei h) {
    viewport_.x = static_cast<float>(x);
    viewport_.y = static_cast<float>(y);
    viewport_.width = static_cast<float>(w);
    viewport_.height = static_cast<float>(h);
    viewport_.minDepth = 0.0f;
    viewport_.maxDepth = 1.0f;
}

void WebGLVkContext::scissor(GLint x, GLint y, GLsizei w, GLsizei h) {
    scissor_.offset.x = std::max(0, x);
    scissor_.offset.y = std::max(0, y);
    scissor_.extent.width = std::max(0, static_cast<int32_t>(w));
    scissor_.extent.height = std::max(0, static_cast<int32_t>(h));
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
    if (cap == GL_BLEND) blendEnabled_ = true;
    else if (cap == GL_DEPTH_TEST) depthTestEnabled_ = true;
    else if (cap == GL_CULL_FACE) cullFaceEnabled_ = true;
    else if (cap == GL_SCISSOR_TEST) scissorTest_ = true;
}

void WebGLVkContext::disable(GLenum cap) {
    if (cap == GL_BLEND) blendEnabled_ = false;
    else if (cap == GL_DEPTH_TEST) depthTestEnabled_ = false;
    else if (cap == GL_CULL_FACE) cullFaceEnabled_ = false;
    else if (cap == GL_SCISSOR_TEST) scissorTest_ = false;
}

GLboolean WebGLVkContext::isEnabled(GLenum cap) {
    if (cap == GL_BLEND) return blendEnabled_;
    if (cap == GL_DEPTH_TEST) return depthTestEnabled_;
    if (cap == GL_CULL_FACE) return cullFaceEnabled_;
    if (cap == GL_SCISSOR_TEST) return scissorTest_;
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
void WebGLVkContext::cullFace(GLenum mode) { cullFaceMode_ = mode; }
void WebGLVkContext::frontFace(GLenum mode) { frontFaceMode_ = mode; }
void WebGLVkContext::lineWidth(GLfloat /*width*/) {}
void WebGLVkContext::polygonOffset(GLfloat /*factor*/, GLfloat /*units*/) {}

// ---------------------------------------------------------------------------
// Buffers
// ---------------------------------------------------------------------------

WebGLBuffer WebGLVkContext::createBuffer() {
    GLuint id = nextBufferId_++;
    buffers_[id] = VkBufferResource{};
    return {id};
}

void WebGLVkContext::deleteBuffer(WebGLBuffer buf) {
    auto it = buffers_.find(buf.id);
    if (it != buffers_.end()) {
        VkDevice dev = context_.device();
        if (it->second.buffer != VK_NULL_HANDLE) vkDestroyBuffer(dev, it->second.buffer, nullptr);
        if (it->second.memory != VK_NULL_HANDLE) vkFreeMemory(dev, it->second.memory, nullptr);
        buffers_.erase(it);
    }
}

void WebGLVkContext::bindBuffer(GLenum target, WebGLBuffer buf) {
    if (target == GL_ARRAY_BUFFER) {
        boundArrayBuffer_ = buf.id;
    } else if (target == GL_ELEMENT_ARRAY_BUFFER) {
        boundElementArrayBuffer_ = buf.id;
        vaos_[currentVaoId_].elementArrayBufferId = buf.id;
    }
}

void WebGLVkContext::bufferData(GLenum target, GLsizeiptr size, const void* data, GLenum /*usage*/) {
    GLuint bufId = (target == GL_ELEMENT_ARRAY_BUFFER) ? boundElementArrayBuffer_ : boundArrayBuffer_;
    if (bufId == 0 || size <= 0) return;

    VkBufferResource& res = buffers_[bufId];
    VkDevice dev = context_.device();

    if (res.buffer != VK_NULL_HANDLE) {
        submitAndFlush();
        vkDestroyBuffer(dev, res.buffer, nullptr);
        vkFreeMemory(dev, res.memory, nullptr);
        res.buffer = VK_NULL_HANDLE;
        res.memory = VK_NULL_HANDLE;
    }

    res.size = static_cast<VkDeviceSize>(size);
    res.shadowData.resize(size);
    if (data) {
        std::memcpy(res.shadowData.data(), data, size);
    } else {
        std::memset(res.shadowData.data(), 0, size);
    }

    VkBufferUsageFlags vkUsage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (target == GL_ARRAY_BUFFER) vkUsage |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    else if (target == GL_ELEMENT_ARRAY_BUFFER) vkUsage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    else vkUsage |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;

    if (!context_.createBuffer(res.size, vkUsage,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                               res.buffer, res.memory)) {
        LOG_ERROR("WebGLVkContext: Failed to allocate VkBuffer (%zu bytes)", size);
        return;
    }

    // Upload initial data if provided
    void* mapped = nullptr;
    if (vkMapMemory(dev, res.memory, 0, res.size, 0, &mapped) == VK_SUCCESS) {
        std::memcpy(mapped, res.shadowData.data(), size);
        vkUnmapMemory(dev, res.memory);
    }
}

void WebGLVkContext::bufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void* data) {
    GLuint bufId = (target == GL_ELEMENT_ARRAY_BUFFER) ? boundElementArrayBuffer_ : boundArrayBuffer_;
    if (bufId == 0 || !data || size <= 0) return;

    VkBufferResource& res = buffers_[bufId];
    if (!res.isValid() || offset + size > static_cast<GLintptr>(res.size)) return;

    std::memcpy(res.shadowData.data() + offset, data, size);

    void* mapped = nullptr;
    if (vkMapMemory(context_.device(), res.memory, offset, size, 0, &mapped) == VK_SUCCESS) {
        std::memcpy(mapped, data, size);
        vkUnmapMemory(context_.device(), res.memory);
    }
}

void WebGLVkContext::getBufferSubData(GLenum target, GLintptr srcByteOffset, void* dstData, GLsizeiptr length) {
    GLuint bufId = (target == GL_ELEMENT_ARRAY_BUFFER) ? boundElementArrayBuffer_ : boundArrayBuffer_;
    if (bufId == 0 || !dstData || length <= 0) return;

    VkBufferResource& res = buffers_[bufId];
    if (srcByteOffset + length <= static_cast<GLintptr>(res.shadowData.size())) {
        std::memcpy(dstData, res.shadowData.data() + srcByteOffset, length);
    }
}

void* WebGLVkContext::mapBufferRange(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield /*access*/) {
    GLuint bufId = (target == GL_ELEMENT_ARRAY_BUFFER) ? boundElementArrayBuffer_ : boundArrayBuffer_;
    if (bufId == 0 || length <= 0) return nullptr;

    VkBufferResource& res = buffers_[bufId];
    if (!res.isValid() || offset + length > static_cast<GLintptr>(res.size)) return nullptr;

    void* mapped = nullptr;
    if (vkMapMemory(context_.device(), res.memory, offset, length, 0, &mapped) == VK_SUCCESS) {
        res.isMapped = true;
        res.mappedPtr = mapped;
        return mapped;
    }
    return nullptr;
}

bool WebGLVkContext::unmapBuffer(GLenum target) {
    GLuint bufId = (target == GL_ELEMENT_ARRAY_BUFFER) ? boundElementArrayBuffer_ : boundArrayBuffer_;
    if (bufId == 0) return false;

    VkBufferResource& res = buffers_[bufId];
    if (res.isMapped) {
        vkUnmapMemory(context_.device(), res.memory);
        res.isMapped = false;
        res.mappedPtr = nullptr;
        return true;
    }
    return false;
}

void WebGLVkContext::flushMappedBufferRange(GLenum /*target*/, GLintptr /*offset*/, GLsizeiptr /*length*/) {}

GLuint WebGLVkContext::boundBuffer(GLenum target) {
    if (target == GL_ARRAY_BUFFER) return boundArrayBuffer_;
    if (target == GL_ELEMENT_ARRAY_BUFFER) return boundElementArrayBuffer_;
    return 0;
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

// ---------------------------------------------------------------------------
// Shaders & Programs
// ---------------------------------------------------------------------------

WebGLShader WebGLVkContext::createShader(GLenum type) {
    GLuint id = nextShaderId_++;
    shaders_[id] = VkShaderResource{type};
    return {id, type};
}

void WebGLVkContext::deleteShader(WebGLShader s) {
    auto it = shaders_.find(s.id);
    if (it != shaders_.end()) {
        it->second.deleteStatus = true;
        if (it->second.attachCount == 0) {
            if (it->second.module != VK_NULL_HANDLE) {
                vkDestroyShaderModule(context_.device(), it->second.module, nullptr);
            }
            shaders_.erase(it);
        }
    }
}

void WebGLVkContext::shaderSource(WebGLShader s, const std::string& src) {
    if (shaders_.find(s.id) != shaders_.end()) {
        shaders_[s.id].source = src;
    }
}

void WebGLVkContext::compileShader(WebGLShader s) {
    auto it = shaders_.find(s.id);
    if (it == shaders_.end()) return;

    VkShaderResource& sh = it->second;
    TranslatedShader tr = WebGLVkShaderCompiler::translateToVulkanGLSL(sh.source, sh.type);
    sh.translatedSource = tr.source;

    VkShaderStageFlagBits stage = (sh.type == GL_VERTEX_SHADER)
        ? VK_SHADER_STAGE_VERTEX_BIT
        : VK_SHADER_STAGE_FRAGMENT_BIT;

    sh.spirv = WebGLVkShaderCompiler::compileToSpirv(sh.translatedSource, stage, &sh.infoLog);
    if (!sh.spirv.empty()) {
        sh.module = WebGLVkShaderCompiler::createShaderModule(context_.device(), sh.spirv);
        sh.compileStatus = (sh.module != VK_NULL_HANDLE);
    } else {
        sh.compileStatus = false;
    }
}

GLint WebGLVkContext::getShaderParameter(WebGLShader s, GLenum pname) {
    auto it = shaders_.find(s.id);
    if (it == shaders_.end()) return 0;
    if (pname == GL_COMPILE_STATUS) return it->second.compileStatus ? GL_TRUE : GL_FALSE;
    if (pname == GL_SHADER_TYPE) return static_cast<GLint>(it->second.type);
    if (pname == GL_DELETE_STATUS) return it->second.deleteStatus ? GL_TRUE : GL_FALSE;
    return 0;
}

std::string WebGLVkContext::getShaderInfoLog(WebGLShader s) {
    auto it = shaders_.find(s.id);
    return (it != shaders_.end()) ? it->second.infoLog : "";
}

WebGLProgram WebGLVkContext::createProgram() {
    GLuint id = nextProgramId_++;
    programs_[id] = VkProgramResource{};
    return {id};
}

void WebGLVkContext::deleteProgram(WebGLProgram p) {
    auto it = programs_.find(p.id);
    if (it != programs_.end()) {
        if (it->second.descriptorSet != VK_NULL_HANDLE && descriptorPool_ != VK_NULL_HANDLE) {
            vkFreeDescriptorSets(context_.device(), descriptorPool_, 1, &it->second.descriptorSet);
        }
        if (it->second.vertShaderId != 0) detachShader(p, {it->second.vertShaderId});
        if (it->second.fragShaderId != 0) detachShader(p, {it->second.fragShaderId});
        programs_.erase(it);
    }
    if (currentProgramId_ == p.id) currentProgramId_ = 0;
}

void WebGLVkContext::attachShader(WebGLProgram p, WebGLShader s) {
    auto itP = programs_.find(p.id);
    auto itS = shaders_.find(s.id);
    if (itP != programs_.end() && itS != shaders_.end()) {
        if (itS->second.type == GL_VERTEX_SHADER) itP->second.vertShaderId = s.id;
        else if (itS->second.type == GL_FRAGMENT_SHADER) itP->second.fragShaderId = s.id;
        itS->second.attachCount++;
    }
}

void WebGLVkContext::detachShader(WebGLProgram p, WebGLShader s) {
    auto it = programs_.find(p.id);
    if (it != programs_.end()) {
        GLuint sId = 0;
        if (it->second.vertShaderId == s.id) { sId = s.id; it->second.vertShaderId = 0; }
        if (it->second.fragShaderId == s.id) { sId = s.id; it->second.fragShaderId = 0; }
        if (sId != 0) {
            auto itS = shaders_.find(sId);
            if (itS != shaders_.end()) {
                if (itS->second.attachCount > 0) itS->second.attachCount--;
                if (itS->second.deleteStatus && itS->second.attachCount == 0) {
                    if (itS->second.module != VK_NULL_HANDLE) {
                        vkDestroyShaderModule(context_.device(), itS->second.module, nullptr);
                    }
                    shaders_.erase(itS);
                }
            }
        }
    }
}

void WebGLVkContext::linkProgram(WebGLProgram p) {
    auto it = programs_.find(p.id);
    if (it == programs_.end()) return;

    VkProgramResource& prog = it->second;
    auto itV = shaders_.find(prog.vertShaderId);
    auto itF = shaders_.find(prog.fragShaderId);

    if (itV == shaders_.end() || itF == shaders_.end() ||
        !itV->second.compileStatus || !itF->second.compileStatus) {
        prog.linkStatus = false;
        prog.infoLog = "Attached shaders not compiled successfully";
        return;
    }

    prog.vertModule = itV->second.module;
    prog.fragModule = itF->second.module;

    // Merge translated shader metadata (attributes, uniforms, push constants)
    TranslatedShader trV = WebGLVkShaderCompiler::translateToVulkanGLSL(itV->second.source, GL_VERTEX_SHADER);
    TranslatedShader trF = WebGLVkShaderCompiler::translateToVulkanGLSL(itF->second.source, GL_FRAGMENT_SHADER);

    prog.attribLocations = trV.attributeLocations;
    prog.uniforms.clear();
    prog.uniformLocations.clear();

    for (const auto& u : trV.uniforms) {
        prog.uniformLocations[u.name] = u.location;
        prog.uniforms.push_back(u);
    }
    for (const auto& u : trF.uniforms) {
        if (prog.uniformLocations.find(u.name) == prog.uniformLocations.end()) {
            prog.uniformLocations[u.name] = u.location;
            prog.uniforms.push_back(u);
        }
    }

    prog.samplerLocToBinding.clear();
    prog.samplerBindings.clear();
    for (const auto& [name, binding] : trF.samplerBindings) {
        auto locIt = prog.uniformLocations.find(name);
        if (locIt != prog.uniformLocations.end()) {
            prog.samplerLocToBinding[locIt->second] = binding;
            prog.samplerBindings[binding] = 0; // default to texture unit 0
        }
    }

    if (descriptorPool_ != VK_NULL_HANDLE && descriptorSetLayout_ != VK_NULL_HANDLE && prog.descriptorSet == VK_NULL_HANDLE) {
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = descriptorPool_;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &descriptorSetLayout_;
        vkAllocateDescriptorSets(context_.device(), &allocInfo, &prog.descriptorSet);
    }

    prog.uniformBytes.assign(128, 0); // Push constant buffer storage
    prog.linkStatus = true;
}

void WebGLVkContext::useProgram(WebGLProgram p) {
    currentProgramId_ = p.id;
}

GLint WebGLVkContext::getProgramParameter(WebGLProgram p, GLenum pname) {
    auto it = programs_.find(p.id);
    if (it == programs_.end()) return 0;
    if (pname == GL_LINK_STATUS) return it->second.linkStatus ? GL_TRUE : GL_FALSE;
    if (pname == GL_ATTACHED_SHADERS) {
        int count = 0;
        if (it->second.vertShaderId != 0) count++;
        if (it->second.fragShaderId != 0) count++;
        return count;
    }
    return 0;
}

std::string WebGLVkContext::getProgramInfoLog(WebGLProgram p) {
    auto it = programs_.find(p.id);
    return (it != programs_.end()) ? it->second.infoLog : "";
}

// ---------------------------------------------------------------------------
// Uniforms & Attributes
// ---------------------------------------------------------------------------

GLint WebGLVkContext::getAttribLocation(WebGLProgram p, const std::string& name) {
    auto it = programs_.find(p.id);
    if (it == programs_.end()) return -1;
    auto aIt = it->second.attribLocations.find(name);
    return (aIt != it->second.attribLocations.end()) ? aIt->second : -1;
}

WebGLUniformLocation WebGLVkContext::getUniformLocation(WebGLProgram p, const std::string& name) {
    auto it = programs_.find(p.id);
    if (it == programs_.end()) return {-1, 0};
    auto uIt = it->second.uniformLocations.find(name);
    return (uIt != it->second.uniformLocations.end()) ? WebGLUniformLocation{uIt->second, p.id} : WebGLUniformLocation{-1, p.id};
}

void WebGLVkContext::uniform1f(WebGLUniformLocation loc, GLfloat v0) { uniform1fv(loc, 1, &v0); }
void WebGLVkContext::uniform2f(WebGLUniformLocation loc, GLfloat v0, GLfloat v1) {
    GLfloat v[2] = {v0, v1}; uniform2fv(loc, 1, v);
}
void WebGLVkContext::uniform3f(WebGLUniformLocation loc, GLfloat v0, GLfloat v1, GLfloat v2) {
    GLfloat v[3] = {v0, v1, v2}; uniform3fv(loc, 1, v);
}
void WebGLVkContext::uniform4f(WebGLUniformLocation loc, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3) {
    GLfloat v[4] = {v0, v1, v2, v3}; uniform4fv(loc, 1, v);
}
void WebGLVkContext::uniform1i(WebGLUniformLocation loc, GLint v0) {
    if (loc.location < 0) return;
    auto it = programs_.find(currentProgramId_);
    if (it == programs_.end()) return;
    auto sIt = it->second.samplerLocToBinding.find(loc.location);
    if (sIt != it->second.samplerLocToBinding.end()) {
        it->second.samplerBindings[sIt->second] = static_cast<uint32_t>(std::max(0, v0));
        return;
    }
    for (const auto& u : it->second.uniforms) {
        if (u.location == loc.location) {
            if (u.offset + sizeof(GLint) <= it->second.uniformBytes.size()) {
                std::memcpy(it->second.uniformBytes.data() + u.offset, &v0, sizeof(GLint));
            }
            break;
        }
    }
}

void WebGLVkContext::uniform2i(WebGLUniformLocation loc, GLint v0, GLint v1) {
    if (loc.location < 0) return;
    auto it = programs_.find(currentProgramId_);
    if (it == programs_.end()) return;
    GLint v[2] = {v0, v1};
    for (const auto& u : it->second.uniforms) {
        if (u.location == loc.location) {
            if (u.offset + sizeof(v) <= it->second.uniformBytes.size()) {
                std::memcpy(it->second.uniformBytes.data() + u.offset, v, sizeof(v));
            }
            break;
        }
    }
}

void WebGLVkContext::uniform1fv(WebGLUniformLocation loc, GLsizei count, const GLfloat* v) {
    if (loc.location < 0 || !v) return;
    auto it = programs_.find(currentProgramId_);
    if (it == programs_.end()) return;
    for (const auto& u : it->second.uniforms) {
        if (u.location == loc.location) {
            size_t bytes = std::min(static_cast<size_t>(count) * sizeof(GLfloat), static_cast<size_t>(u.size));
            if (u.offset + bytes <= it->second.uniformBytes.size()) {
                std::memcpy(it->second.uniformBytes.data() + u.offset, v, bytes);
            }
            break;
        }
    }
}

void WebGLVkContext::uniform2fv(WebGLUniformLocation loc, GLsizei count, const GLfloat* v) {
    if (loc.location < 0 || !v) return;
    auto it = programs_.find(currentProgramId_);
    if (it == programs_.end()) return;
    for (const auto& u : it->second.uniforms) {
        if (u.location == loc.location) {
            size_t bytes = std::min(static_cast<size_t>(count) * 2 * sizeof(GLfloat), static_cast<size_t>(u.size));
            if (u.offset + bytes <= it->second.uniformBytes.size()) {
                std::memcpy(it->second.uniformBytes.data() + u.offset, v, bytes);
            }
            break;
        }
    }
}

void WebGLVkContext::uniform3fv(WebGLUniformLocation loc, GLsizei count, const GLfloat* v) {
    if (loc.location < 0 || !v) return;
    auto it = programs_.find(currentProgramId_);
    if (it == programs_.end()) return;
    for (const auto& u : it->second.uniforms) {
        if (u.location == loc.location) {
            size_t bytes = std::min(static_cast<size_t>(count) * 3 * sizeof(GLfloat), static_cast<size_t>(u.size));
            if (u.offset + bytes <= it->second.uniformBytes.size()) {
                std::memcpy(it->second.uniformBytes.data() + u.offset, v, bytes);
            }
            break;
        }
    }
}

void WebGLVkContext::uniform4fv(WebGLUniformLocation loc, GLsizei count, const GLfloat* v) {
    if (loc.location < 0 || !v) return;
    auto it = programs_.find(currentProgramId_);
    if (it == programs_.end()) return;
    for (const auto& u : it->second.uniforms) {
        if (u.location == loc.location) {
            size_t bytes = std::min(static_cast<size_t>(count) * 4 * sizeof(GLfloat), static_cast<size_t>(u.size));
            if (u.offset + bytes <= it->second.uniformBytes.size()) {
                std::memcpy(it->second.uniformBytes.data() + u.offset, v, bytes);
            }
            break;
        }
    }
}

void WebGLVkContext::uniformMatrix3fv(WebGLUniformLocation loc, GLsizei count, GLboolean /*transpose*/, const GLfloat* value) {
    if (loc.location < 0 || !value) return;
    auto it = programs_.find(currentProgramId_);
    if (it == programs_.end()) return;
    for (const auto& u : it->second.uniforms) {
        if (u.location == loc.location) {
            size_t bytes = std::min(static_cast<size_t>(count) * 48, static_cast<size_t>(u.size));
            if (u.offset + bytes <= it->second.uniformBytes.size()) {
                std::memcpy(it->second.uniformBytes.data() + u.offset, value, bytes);
            }
            break;
        }
    }
}

void WebGLVkContext::uniformMatrix4fv(WebGLUniformLocation loc, GLsizei count, GLboolean /*transpose*/, const GLfloat* value) {
    if (loc.location < 0 || !value) return;
    auto it = programs_.find(currentProgramId_);
    if (it == programs_.end()) return;
    for (const auto& u : it->second.uniforms) {
        if (u.location == loc.location) {
            size_t bytes = std::min(static_cast<size_t>(count) * 64, static_cast<size_t>(u.size));
            if (u.offset + bytes <= it->second.uniformBytes.size()) {
                std::memcpy(it->second.uniformBytes.data() + u.offset, value, bytes);
            }
            break;
        }
    }
}

} // namespace bro::webgl::vk
