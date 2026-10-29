#include "scene/vulkan/shadow/pass_shadow.h"

#include "scene/vulkan/frame/scene_frame.h"
#include "scene/vulkan/frame/scene_gpu_timer.h"
#include "scene/vulkan/frame/scene_view.h"
#include "scene/vulkan/frame/scene_vk_device.h"
#include "scene/vulkan/shadow/scene_caster_drawer.h"
#include "scene/vulkan/shadow/scene_shadow_pages.h"

namespace bro::scene::vk {

namespace {

// Slope-scaled polygon offset on the conventional [0,1] shadow depth: the big
// hammer against acne across the slopes a light sees. The receivers add a
// texel-sized normal offset and bias on top (scene_lighting.glsl).
constexpr float kDepthBiasConstant = 4.0f;
constexpr float kDepthBiasSlope = 2.0f;

/// A view's rect of the scratch target, its pages and their border.
VkRect2D viewRect(const ShadowViewPlan& v) {
    return {{v.scratchX, v.scratchY}, {static_cast<uint32_t>(v.texelsW), static_cast<uint32_t>(v.texelsH)}};
}

}  // namespace

bool PassShadow::active(const SceneFrame& frame) const {
    // Pages that failed to allocate draw nothing.
    const ShadowPlan& plan = frame.renderer.shadowPlan();
    return frame.gpu.shadows.ready() && plan.viewCount > 0;
}

void PassShadow::declare(const SceneFrame& frame, PassIO& io) const {
    io.depthTarget(frame.gpu.shadows.scratch);
}

void PassShadow::drawView(SceneFrame& frame, int index) {
    VkCommandBuffer cmd = frame.cmd;
    const ShadowPlan& plan = frame.renderer.shadowPlan();
    const ShadowViewPlan& view = plan.views[index];
    const VkRect2D rect = viewRect(view);
    const VkViewport viewport{static_cast<float>(rect.offset.x), static_cast<float>(rect.offset.y),
                              static_cast<float>(rect.extent.width), static_cast<float>(rect.extent.height), 0.0f, 1.0f};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &rect);
    // The draws are relative to the frame's eye, the view to the plan's
    // (the same eye unless a caller re-planned in between).
    const bromath::Mat4 lightViewProj = toVulkanClip(rebased(view.relViewProj, frame.view.eye - plan.origin));
    const bool culling = frame.renderer.cullingFrustum() != nullptr;
    // The timed passes' names: the overlays' casters by kind and the pages'
    // by kind, a run of draws under one name marked once as it ends.
    static constexpr const char* kOverlay[kMeshKindCount] = {
        "shadow:moving:static", "shadow:moving:instanced", "shadow:moving:skinned", "shadow:moving:scatter",
        "shadow:moving:tube",   "shadow:moving:veg",       "shadow:moving:instancedVeg"};
    static constexpr const char* kPages[kMeshKindCount] = {
        "shadow:pages:static", "shadow:pages:instanced", "shadow:pages:skinned", "shadow:pages:scatter",
        "shadow:pages:tube",   "shadow:pages:veg",       "shadow:pages:instancedVeg"};
    const char* run = nullptr;
    for (const MeshDraw& draw : frame.lists.meshes) {
        if (!draw.castsShadow || !shadowCastsInto(view, draw.shadowMotion)) continue;
        if (culling && draw.hasBounds && !bromath::fintersects(view.frustum, draw.bounds)) {
            frame.stats.shadowCulled++;
            continue;
        }
        frame.stats.shadowDrawn++;
        const char* label = (view.dynamic ? kOverlay : kPages)[static_cast<int>(draw.kind)];
        if (frame.timer && run && run != label) frame.timer->mark(cmd, run);
        run = label;
        frame.gpu.casters.draw(frame, cmd, lightViewProj, draw, culling ? &view.frustum : nullptr);
    }
    if (frame.timer && run) frame.timer->mark(cmd, run);
}

void PassShadow::record(SceneFrame& frame) {
    VkCommandBuffer cmd = frame.cmd;
    const ShadowPlan& plan = frame.renderer.shadowPlan();
    const SceneVkImage& scratch = frame.gpu.shadows.scratch;

    VkRenderingAttachmentInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth.imageView = scratch.view;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    // Each view's rect is cleared as it is drawn.
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    info.renderArea = {{0, 0}, {scratch.width, scratch.height}};
    info.layerCount = 1;
    info.pDepthAttachment = &depth;
    frame.gpu.device.cmdBeginRendering(cmd, &info);
    vkCmdSetDepthBias(cmd, kDepthBiasConstant, 0.0f, kDepthBiasSlope);

    const auto clearView = [&](int v) {
        VkClearAttachment clear{};
        clear.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        clear.clearValue.depthStencil = {1.0f, 0};
        const VkClearRect clearRect{viewRect(plan.views[v]), 0, 1};
        vkCmdClearAttachments(cmd, 1, &clear, 1, &clearRect);
    };
    // The overlays: what moves, alone.
    for (int v = 0; v < plan.viewCount; ++v) {
        if (!plan.views[v].dynamic) continue;
        clearView(v);
        drawView(frame, v);
    }
    if (frame.timer) frame.timer->mark(cmd, "shadow:moving");

    // The pages redrawn into the cache, all their casters at once: the
    // world service's jobs, each kind's views timed as its span with their
    // pages' units (a page; a page every plant draws into, its
    // ShadowPageWant::units).
    if (plan.staticPages > 0) {
        const auto drawKind = [&](int kind, bool plants, uint32_t pages, double units) {
            if (pages == 0) return;
            const bool stamped =
                frame.timer && kind >= 0 && frame.timer->beginSpan(cmd, static_cast<uint32_t>(kind), false);
            for (int v = 0; v < plan.viewCount; ++v) {
                if (plan.views[v].dynamic || (plan.views[v].plants == ShadowPlants::All) != plants) continue;
                clearView(v);
                drawView(frame, v);
            }
            if (stamped) frame.timer->endSpan(cmd, static_cast<uint32_t>(kind), false, units);
        };
        const uint32_t stillPages = plan.staticPages - plan.plantPages;
        drawKind(frame.gpu.shadows.stillKind(), false, stillPages, stillPages);
        drawKind(frame.gpu.shadows.kind(), true, plan.plantPages, plan.plantUnits);
        if (frame.timer) frame.timer->mark(cmd, "shadow:pages");
    }

    frame.gpu.device.cmdEndRendering(cmd);
}

}  // namespace bro::scene::vk
