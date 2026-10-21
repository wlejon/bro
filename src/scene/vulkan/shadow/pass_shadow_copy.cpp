#include "scene/vulkan/shadow/pass_shadow_copy.h"

#include "scene/vulkan/frame/scene_frame.h"
#include "scene/vulkan/frame/scene_gpu_timer.h"
#include "scene/vulkan/shadow/scene_shadow_pages.h"

#include <vector>

namespace bro::scene::vk {

bool PassShadowCopy::active(const SceneFrame& frame) const {
    return frame.gpu.shadows.ready() && !frame.renderer.shadowPlan().copyOut.empty();
}

void PassShadowCopy::declare(const SceneFrame& frame, PassIO& io) const {
    io.transferSrc(frame.gpu.shadows.scratch);
    io.transferDst(frame.gpu.shadows.pool);
}

void PassShadowCopy::record(SceneFrame& frame) {
    const ShadowPlan& plan = frame.renderer.shadowPlan();
    SceneShadowPages& s = frame.gpu.shadows;
    std::vector<VkImageCopy> regions;
    regions.reserve(plan.copyOut.size());
    for (const bool plants : {false, true}) {
        regions.clear();
        for (const ShadowPageCopy& c : plan.copyOut) {
            if (c.plants != plants) continue;
            VkImageCopy r{};
            r.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
            r.dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
            r.srcOffset = {c.scratchX, c.scratchY, 0};
            r.dstOffset = {(c.physical % plan.poolColumns) * SP_STRIDE, (c.physical / plan.poolColumns) * SP_STRIDE, 0};
            r.extent = {SP_STRIDE, SP_STRIDE, 1};
            regions.push_back(r);
        }
        if (regions.empty()) continue;
        const int kind = plants ? s.kind() : s.stillKind();
        // (Its pages were counted as the pass drew them: no jobs here.)
        const bool stamped =
            frame.timer && kind >= 0 && frame.timer->beginSpan(frame.cmd, static_cast<uint32_t>(kind), false);
        vkCmdCopyImage(frame.cmd, s.scratch.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.pool.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<uint32_t>(regions.size()), regions.data());
        if (stamped) frame.timer->endSpan(frame.cmd, static_cast<uint32_t>(kind), false, 0);
    }
}

}  // namespace bro::scene::vk
