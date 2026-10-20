#include "scene/vulkan/scene_frame_graph.h"

#include "render/vulkan_util.h"
#include "scene/vulkan/scene_frame.h"
#include "scene/vulkan/scene_gpu_timer.h"
#include "scene/vulkan/scene_targets.h"
#include "scene/vulkan/scene_vk_depth.h"
#include "scene/vulkan/scene_vk_device.h"
#include "util/log.h"

namespace bro::scene::vk {

namespace {

VkImageLayout layoutFor(ImageAccess access) {
    switch (access) {
    case ImageAccess::Sampled: return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    case ImageAccess::ColorTarget: return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    case ImageAccess::DepthTarget: return VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    case ImageAccess::TransferSrc: return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    case ImageAccess::TransferDst: return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    case ImageAccess::Storage: return VK_IMAGE_LAYOUT_GENERAL;
    }
    return VK_IMAGE_LAYOUT_GENERAL;
}

VkRenderingAttachmentInfo attachment(const SceneVkImage& drawn, const SceneVkImage* resolve,
                                     VkResolveModeFlagBits mode, VkImageLayout layout, bool clear,
                                     VkClearValue clearValue) {
    VkRenderingAttachmentInfo a{};
    a.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    a.imageView = drawn.view;
    a.imageLayout = layout;
    a.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a.clearValue = clearValue;
    if (resolve) {
        a.resolveMode = mode;
        a.resolveImageView = resolve->view;
        a.resolveImageLayout = layout;
    }
    return a;
}

}  // namespace

VkCommandBuffer SceneFrame::computeStream() {
    if (computeCmd != VK_NULL_HANDLE) return computeCmd;
    computeCmd = gpu.device.computeCommands(cmd);
    if (timer && computeApart()) timer->beginCompute(computeCmd);
    return computeCmd;
}

void SceneFrame::joinCompute() {
    if (computeJoined || !computeApart()) return;
    computeJoined = true;
    cmd = gpu.device.joinCompute(cmd);
}

ScenePass& SceneFrameGraph::add(std::unique_ptr<ScenePass> pass) {
    passes_.push_back(std::move(pass));
    return *passes_.back();
}

bool SceneFrameGraph::setup(SceneGpu& gpu) {
    for (auto& pass : passes_) {
        if (!pass->setup(gpu)) {
            LOG_ERROR("SceneFrameGraph: Failed setting up the %s pass", pass->name());
            return false;
        }
    }
    return true;
}

void SceneFrameGraph::resize(SceneGpu& gpu, uint32_t width, uint32_t height) {
    for (auto& pass : passes_) pass->resize(gpu, width, height);
}

void SceneFrameGraph::releaseNodes(SceneGpu& gpu, std::span<const uint32_t> ids) {
    for (auto& pass : passes_) pass->releaseNodes(gpu, ids);
}

void SceneFrameGraph::cleanup(SceneGpu& gpu) {
    for (auto it = passes_.rbegin(); it != passes_.rend(); ++it) (*it)->cleanup(gpu);
    passes_.clear();
}

void SceneFrameGraph::transition(VkCommandBuffer cmd, SceneVkImage& image, VkImageLayout layout) {
    if (image.currentLayout == layout) return;
    const VkImageSubresourceRange range{render::imageAspectFor(image.format), 0, image.mipLevels, 0,
                                        image.arrayLayers};
    render::cmdTransitionImage(cmd, image.image, range, image.currentLayout, layout);
    image.currentLayout = layout;
}

void SceneFrameGraph::run(SceneFrame& frame) {
    scopeOpen_ = false;
    firstScope_ = true;
    indirectCleared_ = false;

    for (size_t i = 0; i < passes_.size(); ++i) {
        ScenePass& pass = *passes_[i];
        if (!pass.active(frame)) continue;
        PassIO io;
        pass.declare(frame, io);

        // The frame's first reader of its compute results: the graphics is
        // split here, the rest waiting for the compute (outside rendering).
        const bool join = io.readsComputeResults() && frame.computeApart() && !frame.computeJoined;
        if (scopeOpen_ && (join || moves(io) || !io.drawsHdr() || io.scope().indirect != scopeIndirect_))
            closeScope(frame);
        if (join) frame.joinCompute();
        for (const auto& use : io.uses()) transition(frame.cmd, *use.image, layoutFor(use.access));
        if (io.drawsHdr() && !scopeOpen_) openScope(frame, io.scope(), resolves(frame, i, io.scope()));

        pass.record(frame);
        if (frame.timer) frame.timer->mark(frame.cmd, pass.name());
    }
    if (scopeOpen_) closeScope(frame);
}

bool SceneFrameGraph::moves(const PassIO& io) {
    for (const auto& use : io.uses()) {
        if (use.image->currentLayout != layoutFor(use.access)) return true;
    }
    return false;
}

bool SceneFrameGraph::resolves(const SceneFrame& frame, size_t at, const HdrScope& scope) const {
    if (!frame.gpu.targets.msaa()) return true;
    bool closed = false;
    for (size_t j = at + 1; j < passes_.size(); ++j) {
        const ScenePass& pass = *passes_[j];
        if (!pass.active(frame)) continue;
        PassIO io;
        pass.declare(frame, io);
        // (The passes sharing the scope move no image, so the layouts now
        // are the ones the pass that closes it meets.)
        if (!closed && io.drawsHdr() && !moves(io) && io.scope().indirect == scope.indirect) continue;
        // (Its resolve takes in this one's attachments: all of them only if
        // it has the same.)
        if (closed && io.drawsHdr()) return io.scope().indirect != scope.indirect;
        if (!io.readsUnresolved()) return true;
        closed = true;
    }
    return true;
}

void SceneFrameGraph::openScope(SceneFrame& frame, const HdrScope& scope, bool resolve) {
    SceneTargets& t = frame.gpu.targets;
    VkCommandBuffer cmd = frame.cmd;
    const bool msaa = t.msaa();
    const bool resolving = msaa && resolve;
    constexpr VkImageLayout kColor = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    constexpr VkImageLayout kDepth = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;

    transition(cmd, t.hdrDrawn(), kColor);
    transition(cmd, t.depthDrawn(), kDepth);
    if (msaa) {
        transition(cmd, t.hdr, kColor);
        transition(cmd, t.depth, kDepth);
    }
    if (scope.indirect) {
        transition(cmd, t.indirectDrawn(), kColor);
        if (msaa) transition(cmd, t.indirect, kColor);
    }

    VkClearValue clearColor{};
    VkClearValue clearDepth{};
    clearDepth.depthStencil = {depth::clearFar(), 0};

    VkRenderingAttachmentInfo colors[2];
    colors[0] = attachment(t.hdrDrawn(), resolving ? &t.hdr : nullptr, VK_RESOLVE_MODE_AVERAGE_BIT, kColor,
                           firstScope_, clearColor);
    uint32_t colorCount = 1;
    if (scope.indirect) {
        colors[1] = attachment(t.indirectDrawn(), resolving ? &t.indirect : nullptr, VK_RESOLVE_MODE_AVERAGE_BIT,
                               kColor, !indirectCleared_, clearColor);
        colorCount = 2;
        indirectCleared_ = true;
    }
    VkRenderingAttachmentInfo depthAtt = attachment(t.depthDrawn(), resolving ? &t.depth : nullptr,
                                                    t.depthResolve(), kDepth, firstScope_, clearDepth);

    VkRenderingInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    info.renderArea = {{0, 0}, {t.width(), t.height()}};
    info.layerCount = 1;
    info.colorAttachmentCount = colorCount;
    info.pColorAttachments = colors;
    info.pDepthAttachment = &depthAtt;
    frame.gpu.device.cmdBeginRendering(cmd, &info);

    const VkViewport viewport{0.0f, 0.0f, static_cast<float>(t.width()), static_cast<float>(t.height()), 0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, {t.width(), t.height()}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    frame.hdrTarget = t.hdrTarget(scope.indirect);
    scopeOpen_ = true;
    scopeIndirect_ = scope.indirect;
    firstScope_ = false;
}

void SceneFrameGraph::closeScope(SceneFrame& frame) {
    frame.gpu.device.cmdEndRendering(frame.cmd);
    // Attachment writes and resolves (a depth resolve runs in the colour
    // output stage) are visible to anything later in the frame.
    render::cmdMemoryBarrier(frame.cmd,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                 VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                 VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                             VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
    scopeOpen_ = false;
}

}  // namespace bro::scene::vk
