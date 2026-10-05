// The draw calls: their validation, the vertex and index ranges they fetch,
// and the index rewrites Vulkan needs where GL draws something it has no
// direct form of: 8-bit indices, LINE_LOOP, and primitive restart in list
// topologies on devices without VK_EXT_primitive_topology_list_restart.
// WebGL 2 restarts every indexed draw at the type's largest index.

#include "webgl/vulkan/webgl_vk_context.h"
#include "util/log.h"

#include <algorithm>
#include <cstring>

namespace bro::webgl::vk {

namespace {

constexpr uint32_t kRestart32 = 0xFFFFFFFFu;

uint32_t indexBytes(GLenum type) {
    return type == GL_UNSIGNED_BYTE ? 1 : type == GL_UNSIGNED_SHORT ? 2 : 4;
}

uint32_t restartIndex(GLenum type) {
    return type == GL_UNSIGNED_BYTE ? 0xFFu : type == GL_UNSIGNED_SHORT ? 0xFFFFu : kRestart32;
}

uint32_t readIndex(const uint8_t* src, GLenum type, uint32_t i) {
    switch (type) {
        case GL_UNSIGNED_BYTE: return src[i];
        case GL_UNSIGNED_SHORT: { uint16_t v; std::memcpy(&v, src + i * 2, 2); return v; }
        default: { uint32_t v; std::memcpy(&v, src + i * 4, 4); return v; }
    }
}

// Vertices per primitive of a list topology; 0 for strips, fans and loops.
uint32_t listVertices(GLenum mode) {
    switch (mode) {
        case GL_POINTS: return 1;
        case GL_LINES: return 2;
        case GL_TRIANGLES: return 3;
        default: return 0;
    }
}

} // namespace

bool WebGLVkContext::drawModeValid(GLenum mode) {
    if (mode <= GL_TRIANGLE_FAN) return true;
    setSyntheticError(GL_INVALID_ENUM);
    return false;
}

// The index range `count` indices of `type` at `offset` span, from the
// buffer's cache while its contents are those it was scanned at.
bool WebGLVkContext::indexRange(VkBufferResource& ibo, uintptr_t offset, uint32_t count, GLenum type,
                                IndexRange& out) {
    for (const IndexRange& r : ibo.indexRanges) {
        if (r.version == ibo.version && r.offset == offset && r.count == count && r.type == type) {
            out = r;
            return true;
        }
    }
    syncShadow(ibo);
    IndexRange r;
    r.version = ibo.version;
    r.offset = offset;
    r.count = count;
    r.type = type;
    r.minIndex = kRestart32;
    const uint8_t* src = ibo.shadowData.data() + offset;
    const uint32_t restart = restartIndex(type);
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t v = readIndex(src, type, i);
        if (v == restart) {
            r.restart = true;
            continue;
        }
        r.minIndex = std::min(r.minIndex, v);
        r.maxIndex = std::max(r.maxIndex, v);
        r.empty = false;
    }
    if (r.empty) r.minIndex = 0;
    ibo.indexRanges[ibo.nextIndexRange] = r;
    ibo.nextIndexRange = (ibo.nextIndexRange + 1) % ibo.indexRanges.size();
    out = r;
    return true;
}

void WebGLVkContext::drawIndices(GLenum mode, VkProgramResource& prog, VkBuffer buffer, VkDeviceSize offset,
                                 VkIndexType type, uint32_t count, DrawShape& shape) {
    if (!prepareDraw(mode, prog, shape)) return;
    VkCommandBuffer cmd = commands();
    vkCmdBindIndexBuffer(cmd, buffer, offset, type);
    vkCmdDrawIndexed(cmd, count, shape.instances, 0, 0, 0);
}

void WebGLVkContext::drawArrays(GLenum mode, GLint first, GLsizei count) {
    drawArraysInstanced(mode, first, count, 1);
}

void WebGLVkContext::drawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instanceCount) {
    if (!drawModeValid(mode)) return;
    if (first < 0 || count < 0 || instanceCount < 0) {
        setSyntheticError(GL_INVALID_VALUE);
        return;
    }
    VkProgramResource* prog = drawProgram("drawArrays");
    if (!prog) return;
    const uint64_t end = static_cast<uint64_t>(first) + static_cast<uint64_t>(count);
    if (end > 0x7FFFFFFFu) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    if (count == 0 || instanceCount == 0) return;
    flushIfOverBudget();
    DrawShape shape;
    shape.first = static_cast<uint32_t>(first);
    shape.end = static_cast<uint32_t>(end);
    shape.instances = static_cast<uint32_t>(instanceCount);
    if (feedbackCapturing() && !prepareFeedback(mode, shape.end - shape.first, shape.instances, shape)) return;

    if (mode == GL_LINE_LOOP) {
        // A strip through the vertices and back to the first.
        const uint32_t n = static_cast<uint32_t>(count);
        render::UploadSlice slice = stage(nullptr, static_cast<VkDeviceSize>(n + 1) * 4, 4);
        if (!slice) return;
        auto* idx = static_cast<uint32_t*>(slice.mapped);
        for (uint32_t i = 0; i < n; ++i) idx[i] = shape.first + i;
        idx[n] = shape.first;
        drawIndices(mode, *prog, slice.buffer, slice.offset, VK_INDEX_TYPE_UINT32, n + 1, shape);
        return;
    }
    if (!prepareDraw(mode, *prog, shape)) return;
    vkCmdDraw(commands(), static_cast<uint32_t>(count), shape.instances, shape.first, 0);
    if (shape.feedback) feedbackDrawn(mode, shape);
}

void WebGLVkContext::drawElements(GLenum mode, GLsizei count, GLenum type, uintptr_t offset) {
    drawElementsInstanced(mode, count, type, offset, 1);
}

void WebGLVkContext::drawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type,
                                       uintptr_t offset) {
    if (end < start) {
        setSyntheticError(GL_INVALID_VALUE);
        return;
    }
    drawElementsInstanced(mode, count, type, offset, 1);
}

void WebGLVkContext::drawElementsInstanced(GLenum mode, GLsizei count, GLenum type, uintptr_t offset,
                                           GLsizei instanceCount) {
    if (!drawModeValid(mode)) return;
    if (type != GL_UNSIGNED_BYTE && type != GL_UNSIGNED_SHORT && type != GL_UNSIGNED_INT) {
        setSyntheticError(GL_INVALID_ENUM);
        return;
    }
    if (count < 0 || instanceCount < 0) {
        setSyntheticError(GL_INVALID_VALUE);
        return;
    }
    const uint32_t bytes = indexBytes(type);
    // ES 3.0 captures drawArrays* only.
    if (offset % bytes != 0 || feedbackCapturing()) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    VkProgramResource* prog = drawProgram("drawElements");
    if (!prog) return;
    const GLuint iboId = vaos_[currentVaoId_].elementArrayBufferId;
    auto iboIt = iboId != 0 ? buffers_.find(iboId) : buffers_.end();
    if (iboIt == buffers_.end()) {
        LOG_ERROR("WebGLVkContext: drawElements called with no element array buffer bound");
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    VkBufferResource& ibo = iboIt->second;
    if (offset + static_cast<uint64_t>(count) * bytes > ibo.shadowData.size()) {
        LOG_ERROR("WebGLVkContext: drawElements reads past the end of the element array buffer");
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    if (count == 0 || instanceCount == 0) return;
    flushIfOverBudget();

    const uint32_t n = static_cast<uint32_t>(count);
    IndexRange range;
    indexRange(ibo, offset, n, type, range);
    if (range.empty) return;
    DrawShape shape;
    shape.first = range.minIndex;
    shape.end = range.maxIndex + 1;
    shape.instances = static_cast<uint32_t>(instanceCount);

    const uint32_t perPrimitive = listVertices(mode);
    const bool unrestartable = range.restart && perPrimitive > 0 && !context_.primitiveListRestart();
    if (type != GL_UNSIGNED_BYTE && mode != GL_LINE_LOOP && !unrestartable) {
        shape.restart = range.restart;
        drawIndices(mode, *prog, ibo.buffer, offset,
                    type == GL_UNSIGNED_SHORT ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32, n, shape);
        return;
    }

    // Rewrite the indices as 32-bit ones in this draw's own upload memory,
    // the restart index as Vulkan's: a loop closes each restart-separated
    // strip, and a list without restart support keeps each segment's whole
    // primitives (GL drops a partial one at a restart).
    const uint8_t* src = ibo.shadowData.data() + offset;
    const uint32_t restart = restartIndex(type);
    std::vector<uint32_t> out;
    out.reserve(n + n / 2 + 1);
    uint32_t segment = 0;
    while (segment < n) {
        uint32_t segEnd = segment;
        while (segEnd < n && readIndex(src, type, segEnd) != restart) ++segEnd;
        uint32_t len = segEnd - segment;
        if (unrestartable) len -= len % perPrimitive;
        const bool loop = mode == GL_LINE_LOOP && len >= 2;
        if (len > 0 && (mode != GL_LINE_LOOP || loop)) {
            if (!unrestartable && !out.empty()) out.push_back(kRestart32);
            for (uint32_t i = segment; i < segment + len; ++i) out.push_back(readIndex(src, type, i));
            if (loop) out.push_back(readIndex(src, type, segment));
        }
        segment = segEnd + 1;
    }
    if (out.empty()) return;
    shape.restart = !unrestartable && std::find(out.begin(), out.end(), kRestart32) != out.end();
    render::UploadSlice slice = stage(out.data(), out.size() * sizeof(uint32_t), 4);
    if (!slice) return;
    drawIndices(mode, *prog, slice.buffer, slice.offset, VK_INDEX_TYPE_UINT32, static_cast<uint32_t>(out.size()),
                shape);
}

} // namespace bro::webgl::vk
