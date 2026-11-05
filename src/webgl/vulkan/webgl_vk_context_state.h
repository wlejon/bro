#pragma once

// Plain state records of WebGLVkContext: what a pass renders into, what a
// draw fetches, transform feedback and query objects, and what the open
// pass's command buffer has bound (so a draw records only what changed).

#include "webgl/vulkan/webgl_vk_types.h"

#include <vulkan/vulkan.h>
#include <array>
#include <cstdint>
#include <vector>

namespace bro::webgl::vk {

// A Surface is one image a framebuffer reads or writes: a texture
// level/layer, a renderbuffer, or the canvas's color or depth image.
// Textures stay sampleable between passes (a pass moves its attachments to
// attachment layout and back); renderbuffers and the canvas stay wherever
// they were last used.
struct PassSurface {
    enum class Source : uint8_t { None, Texture, Renderbuffer, CanvasColor, CanvasDepth };
    Source source = Source::None;
    VkTextureResource* tex = nullptr;   // Texture / Renderbuffer storage
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;  // the attached level + layer, every aspect
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0;     // of the attached level
    uint32_t level = 0, layer = 0;      // the subresource (a 3D texture's layer is 0 ...
    int32_t z = 0;                      // ... and its slice is z)
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    bool alphaOne = false;              // an RGB format stored as RGBA: alpha stays one
    explicit operator bool() const { return source != Source::None; }
    // The canvas is stored top-down; framebuffer objects in GL row order.
    bool topDown() const { return source == Source::CanvasColor || source == Source::CanvasDepth; }
    VkImageAspectFlags aspects() const;
};

// What a pass over the draw framebuffer renders into: color[i] is what
// fragment output i writes (DRAW_BUFFERi), empty for NONE.
struct PassTarget {
    std::array<PassSurface, 8> color{};
    uint32_t colorCount = 0;
    PassSurface depth;    // the depth attachment, when it has a depth aspect
    PassSurface stencil;  // the stencil attachment, when it has a stencil aspect
    VkExtent2D extent{};
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    bool topDown = false;
};

// What one draw fetches: vertices [first, end) of the per-vertex arrays
// for `instances` instances, and whether its indices restart primitives.
struct DrawShape {
    uint32_t first = 0, end = 0;
    uint32_t instances = 1;
    bool restart = false;
    bool feedback = false;      // captures (transform feedback active)
    FeedbackPush feedbackPush;  // where its records go
};

// An indexed buffer binding; size 0 is bindBufferBase's whole buffer.
struct IndexedBuffer {
    GLuint buffer = 0;
    GLintptr offset = 0;
    GLsizeiptr size = 0;
};

// A transform feedback object; 0 is the default one.
struct FeedbackObject {
    std::array<IndexedBuffer, kMaxFeedbackBuffers> buffers{};
    bool everBound = false;
    bool active = false, paused = false;
    GLenum primitiveMode = 0;
    GLuint program = 0;    // the program capturing
    uint32_t records = 0;  // vertices captured since beginTransformFeedback
};

// A query object.
struct QueryObject {
    GLenum target = 0;              // 0 until first begun
    std::vector<uint32_t> slots;    // the occlusion slots of the last run
    uint64_t ticket = 0;            // the submission its last use is in
    bool pending = false;           // used in the open command buffer
    uint64_t count = 0;             // samples (summed) or primitives written
    std::vector<uint32_t> stamps;   // timer queries: their timestamp slots
};

/// Vulkan query slots of one type, handed out from 64-slot pools and
/// retired with the ticket of their last use.
struct QuerySlots {
    VkQueryType type = VK_QUERY_TYPE_OCCLUSION;
    std::vector<VkQueryPool> pools;
    std::vector<uint32_t> free;
    std::vector<std::pair<uint64_t, std::vector<uint32_t>>> retired;
    VkQueryPool pool(uint32_t slot) const { return pools[slot / 64]; }
    static uint32_t index(uint32_t slot) { return slot % 64; }
};

/// What the open pass's command buffer has bound and set, so that a draw
/// records only what differs from the draw before it. Command buffer state
/// starts undefined, so the context resets this whenever a pass begins or
/// ends (every command buffer boundary is outside a pass) and after anything
/// that binds or sets state behind its back (an internal clear's draw).
struct PassBindings {
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    uint32_t dynamicOffset = 0;  // the set's default-block offset, when it has one
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    FragmentPush fragmentPush{};
    bool fragmentPushSet = false;

    // Dynamic state (every pipeline takes the same dynamic states, so a
    // value stays set across pipeline binds).
    enum : uint8_t { kViewport = 1, kScissor = 2, kDepthBias = 4, kBlendConstants = 8, kLineWidth = 16 };
    uint8_t dynamicSet = 0;
    VkViewport viewport{};
    VkRect2D scissor{};
    float depthBiasConstant = 0.0f, depthBiasSlope = 0.0f;
    float blendConstants[4]{};
    float lineWidth = 1.0f;

    uint32_t vertexBufferCount = 0;  // bindings [0, count) hold what the arrays say
    std::array<VkBuffer, 16> vertexBuffers{};
    std::array<VkDeviceSize, 16> vertexOffsets{};
    VkBuffer indexBuffer = VK_NULL_HANDLE;
    VkDeviceSize indexOffset = 0;
    VkIndexType indexType = VK_INDEX_TYPE_UINT16;

    void reset() { *this = PassBindings{}; }

    // Record the value unless the command buffer already has it
    // (webgl_vk_context_commands.cpp; the header is also compiled without
    // Vulkan prototypes).
    void setViewport(VkCommandBuffer cmd, const VkViewport& vp);
    void setScissor(VkCommandBuffer cmd, const VkRect2D& sc);
    void setDepthBias(VkCommandBuffer cmd, float constant, float slope);
    void setBlendConstants(VkCommandBuffer cmd, const float c[4]);
    void setLineWidth(VkCommandBuffer cmd, float width);
    /// Bind vertex buffers [0, count): only the run of bindings that differs.
    void bindVertexBuffers(VkCommandBuffer cmd, uint32_t count, const VkBuffer* buffers, const VkDeviceSize* offsets);
    void bindIndexBuffer(VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize offset, VkIndexType type);
};

} // namespace bro::webgl::vk
