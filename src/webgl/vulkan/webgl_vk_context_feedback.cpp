// Transform feedback. Vulkan core has none (VK_EXT_transform_feedback is not
// on every device — MoltenVK lacks it), so a capturing program's vertex
// stage stores its varyings itself: the linker gives it a main that writes
// each captured vertex's record into the bound buffers, bound as storage
// buffers (kFeedbackBinding). ES 3.0 only captures drawArrays* in the
// primitive mode transform feedback began with, so the record of a vertex is
// its index in the draw: records already written + instance * vertices per
// instance + (vertex index - first), and only whole primitives are kept.
//
// A buffer the GPU wrote is newer than its CPU copy; the copy is read back
// before anything reads it (syncShadow).

#include "webgl/vulkan/webgl_vk_context.h"
#include "render/vulkan_util.h"
#include "util/log.h"

#include <algorithm>
#include <cstring>

namespace bro::webgl::vk {

namespace {

uint32_t primitiveVertices(GLenum mode) {
    return mode == GL_TRIANGLES ? 3 : mode == GL_LINES ? 2 : 1;
}

} // namespace

bool WebGLVkContext::transformFeedbackSupported() const {
    return context_.features().vertexPipelineStoresAndAtomics == VK_TRUE;
}

GLuint WebGLVkContext::createTransformFeedback() {
    if (!transformFeedbackSupported()) return 0;
    const GLuint id = nextObjectId_++;
    feedbacks_[id] = FeedbackObject{};
    return id;
}

void WebGLVkContext::deleteTransformFeedback(GLuint id) {
    auto it = id != 0 ? feedbacks_.find(id) : feedbacks_.end();
    if (it == feedbacks_.end()) return;
    if (it->second.active) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    if (boundFeedback_ == id) boundFeedback_ = 0;
    feedbacks_.erase(it);
}

GLboolean WebGLVkContext::isTransformFeedback(GLuint id) const {
    auto it = id != 0 ? feedbacks_.find(id) : feedbacks_.end();
    return it != feedbacks_.end() && it->second.everBound ? GL_TRUE : GL_FALSE;
}

void WebGLVkContext::bindTransformFeedback(GLenum target, GLuint id) {
    if (target != GL_TRANSFORM_FEEDBACK) {
        setSyntheticError(GL_INVALID_ENUM);
        return;
    }
    const FeedbackObject& current = feedbacks_[boundFeedback_];
    auto it = feedbacks_.find(id);
    if ((current.active && !current.paused) || it == feedbacks_.end()) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    it->second.everBound = true;
    boundFeedback_ = id;
}

bool WebGLVkContext::transformFeedbackActive() const {
    auto it = feedbacks_.find(boundFeedback_);
    return it != feedbacks_.end() && it->second.active;
}

bool WebGLVkContext::transformFeedbackPaused() const {
    auto it = feedbacks_.find(boundFeedback_);
    return it != feedbacks_.end() && it->second.active && it->second.paused;
}

bool WebGLVkContext::feedbackCapturing() const {
    auto it = feedbacks_.find(boundFeedback_);
    return it != feedbacks_.end() && it->second.active && !it->second.paused;
}

void WebGLVkContext::beginTransformFeedback(GLenum primitiveMode) {
    if (primitiveMode != GL_POINTS && primitiveMode != GL_LINES && primitiveMode != GL_TRIANGLES) {
        setSyntheticError(GL_INVALID_ENUM);
        return;
    }
    FeedbackObject& f = feedbacks_[boundFeedback_];
    auto progIt = programs_.find(currentProgramId_);
    if (f.active || progIt == programs_.end() || !progIt->second.linkStatus ||
        progIt->second.iface.feedbackVaryings.empty()) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    // Every buffer the program captures into needs a binding.
    for (uint32_t b = 0; b < progIt->second.iface.feedbackBuffers; ++b) {
        if (f.buffers[b].buffer == 0 || buffers_.count(f.buffers[b].buffer) == 0) {
            setSyntheticError(GL_INVALID_OPERATION);
            return;
        }
    }
    f.active = true;
    f.paused = false;
    f.primitiveMode = primitiveMode;
    f.program = currentProgramId_;
    f.records = 0;
}

// What the vertex stage stored is visible to every later use of the buffers.
void WebGLVkContext::feedbackWritesDone() {
    endRendering();
    render::cmdMemoryBarrier(commands(), VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
}

void WebGLVkContext::endTransformFeedback() {
    FeedbackObject& f = feedbacks_[boundFeedback_];
    if (!f.active) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    f.active = false;
    f.paused = false;
    feedbackWritesDone();
}

void WebGLVkContext::pauseTransformFeedback() {
    FeedbackObject& f = feedbacks_[boundFeedback_];
    if (!f.active || f.paused) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    f.paused = true;
    feedbackWritesDone();
}

void WebGLVkContext::resumeTransformFeedback() {
    FeedbackObject& f = feedbacks_[boundFeedback_];
    if (!f.active || !f.paused || f.program != currentProgramId_) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    f.paused = false;
}

// bindBufferBase / bindBufferRange on TRANSFORM_FEEDBACK_BUFFER: the bound
// object's binding `index`. Size 0 is the whole buffer.
bool WebGLVkContext::bindFeedbackBuffer(GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size) {
    if (index >= kMaxFeedbackBuffers || offset < 0 || offset % 4 != 0 || size % 4 != 0) {
        setSyntheticError(GL_INVALID_VALUE);
        return false;
    }
    FeedbackObject& f = feedbacks_[boundFeedback_];
    if (f.active) {
        setSyntheticError(GL_INVALID_OPERATION);
        return false;
    }
    f.buffers[index] = {buffer, buffer != 0 ? offset : 0, buffer != 0 ? size : 0};
    boundTransformFeedbackBuffer_ = buffer;
    return true;
}

void WebGLVkContext::transformFeedbackVaryings(GLuint program, const std::vector<std::string>& varyings,
                                               GLenum bufferMode) {
    auto it = programs_.find(program);
    if (it == programs_.end()) {
        setSyntheticError(GL_INVALID_VALUE);
        return;
    }
    if (bufferMode != GL_INTERLEAVED_ATTRIBS && bufferMode != GL_SEPARATE_ATTRIBS) {
        setSyntheticError(GL_INVALID_ENUM);
        return;
    }
    if (bufferMode == GL_SEPARATE_ATTRIBS && varyings.size() > kMaxFeedbackBuffers) {
        setSyntheticError(GL_INVALID_VALUE);
        return;
    }
    // Takes effect at the next link.
    it->second.feedbackRequest = {varyings, bufferMode};
}

bool WebGLVkContext::getTransformFeedbackVarying(GLuint program, GLuint index, VkFeedbackVarying& out) {
    auto it = programs_.find(program);
    if (it == programs_.end()) {
        setSyntheticError(GL_INVALID_VALUE);
        return false;
    }
    const auto& varyings = it->second.iface.feedbackVaryings;
    if (index >= varyings.size()) {
        setSyntheticError(GL_INVALID_VALUE);
        return false;
    }
    out = varyings[index];
    return true;
}

// The draw's capture, checked (ES 3.0 2.15.2, WebGL 2 5.x): the primitive
// mode transform feedback began with, room for every record in every bound
// buffer, and no captured buffer also read by the draw. Fills the shape's
// capture state.
bool WebGLVkContext::prepareFeedback(GLenum mode, uint32_t count, uint32_t instances, DrawShape& shape) {
    FeedbackObject& f = feedbacks_[boundFeedback_];
    const VkProgramResource& prog = programs_[f.program];
    if (mode != f.primitiveMode) {
        setSyntheticError(GL_INVALID_OPERATION);
        return false;
    }
    const uint32_t perInstance = count - count % primitiveVertices(mode);
    const uint64_t records = static_cast<uint64_t>(perInstance) * instances;
    const VkVAOResource& vao = vaos_[currentVaoId_];
    for (uint32_t b = 0; b < prog.iface.feedbackBuffers; ++b) {
        const IndexedBuffer& binding = f.buffers[b];
        auto bIt = buffers_.find(binding.buffer);
        if (bIt == buffers_.end() || !bIt->second.isValid()) {
            setSyntheticError(GL_INVALID_OPERATION);
            return false;
        }
        const uint64_t size = static_cast<uint64_t>(bIt->second.size);
        const uint64_t offset = static_cast<uint64_t>(binding.offset);
        const uint64_t room = binding.size > 0 ? std::min<uint64_t>(static_cast<uint64_t>(binding.size),
                                                                    size > offset ? size - offset : 0)
                                               : (size > offset ? size - offset : 0);
        const uint64_t needed = (f.records + records) * prog.iface.feedbackStrides[b] * 4;
        if (needed > room) {
            LOG_ERROR("WebGLVkContext: transform feedback buffer %u has no room for the draw's records", b);
            setSyntheticError(GL_INVALID_OPERATION);
            return false;
        }
        for (const VkVertexAttribute& attr : vao.attributes) {
            if (attr.enabled && attr.bufferId == binding.buffer) {
                LOG_ERROR("WebGLVkContext: a transform feedback buffer is also a vertex array of the draw");
                setSyntheticError(GL_INVALID_OPERATION);
                return false;
            }
        }
        for (GLuint point : prog.blockBindings) {
            if (point < boundUniformBuffers_.size() && boundUniformBuffers_[point].buffer == binding.buffer) {
                LOG_ERROR("WebGLVkContext: a transform feedback buffer is also a uniform buffer of the draw");
                setSyntheticError(GL_INVALID_OPERATION);
                return false;
            }
        }
    }
    shape.feedback = true;
    shape.feedbackPush.base = f.records;
    shape.feedbackPush.first = shape.first;
    shape.feedbackPush.count = perInstance;
    return true;
}

// The capture a draw recorded: its records are written, and counted by an
// active TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN query.
void WebGLVkContext::feedbackDrawn(GLenum mode, const DrawShape& shape) {
    FeedbackObject& f = feedbacks_[boundFeedback_];
    const VkProgramResource& prog = programs_[f.program];
    const uint32_t records = shape.feedbackPush.count * shape.instances;
    f.records += records;
    for (uint32_t b = 0; b < prog.iface.feedbackBuffers; ++b) {
        auto bIt = buffers_.find(f.buffers[b].buffer);
        if (bIt == buffers_.end()) continue;
        bIt->second.deviceNewer = true;
        ++bIt->second.version;
    }
    if (activeFeedbackQuery_ != 0) {
        QueryObject& q = queries_[activeFeedbackQuery_];
        q.count += records / primitiveVertices(mode);
        queryUsed(q, activeFeedbackQuery_);
    }
}

// The storage buffer descriptors of a capturing program's bindings: the
// bound ranges while capturing (the descriptor offset aligned down, the rest
// pushed as a word offset), else a placeholder nothing is written to.
void WebGLVkContext::feedbackDescriptors(const VkProgramResource& prog, DrawShape& shape,
                                         std::vector<VkDescriptorBufferInfo>& out) {
    out.clear();
    if (feedbackPlaceholder_.buffer == VK_NULL_HANDLE) {
        void* mapped = nullptr;
        context_.createBuffer(16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                              feedbackPlaceholder_.buffer, feedbackPlaceholder_.memory, feedbackPlaceholder_.offset,
                              feedbackPlaceholder_.allocId, mapped);
    }
    const FeedbackObject& f = feedbacks_[boundFeedback_];
    const VkDeviceSize align = context_.deviceProperties().limits.minStorageBufferOffsetAlignment;
    for (uint32_t b = 0; b < prog.iface.feedbackBuffers; ++b) {
        auto bIt = shape.feedback ? buffers_.find(f.buffers[b].buffer) : buffers_.end();
        if (bIt == buffers_.end()) {
            out.push_back({feedbackPlaceholder_.buffer, 0, VK_WHOLE_SIZE});
            continue;
        }
        const VkDeviceSize offset = static_cast<VkDeviceSize>(f.buffers[b].offset);
        const VkDeviceSize aligned = align > 0 ? offset / align * align : offset;
        shape.feedbackPush.wordOffset[b] = static_cast<uint32_t>((offset - aligned) / 4);
        out.push_back({bIt->second.buffer, aligned, VK_WHOLE_SIZE});
    }
}

void WebGLVkContext::destroyFeedback() {
    if (feedbackPlaceholder_.buffer != VK_NULL_HANDLE)
        context_.destroyBuffer(feedbackPlaceholder_.buffer, feedbackPlaceholder_.allocId);
    feedbackPlaceholder_ = VkBufferResource{};
}

// Bring a buffer's CPU copy up to what the GPU wrote into it.
bool WebGLVkContext::syncShadow(VkBufferResource& res) {
    if (!res.deviceNewer) return true;
    res.deviceNewer = false;
    if (!res.isValid() || res.size == 0) return true;
    void* mapped = readbackMemory(res.size);
    if (!mapped) return false;
    VkCommandBuffer cmd = transferCommands();
    const VkBufferCopy region{0, 0, res.size};
    vkCmdCopyBuffer(cmd, res.buffer, readback_.buffer, 1, &region);
    render::cmdMemoryBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
    if (!waitForCommands()) return false;
    std::memcpy(res.shadowData.data(), mapped, static_cast<size_t>(res.size));
    return true;
}

} // namespace bro::webgl::vk
