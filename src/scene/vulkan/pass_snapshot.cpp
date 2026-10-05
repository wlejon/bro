#include "scene/vulkan/pass_snapshot.h"

#include "scene/particles3d_node.h"
#include "scene/scene_renderer.h"
#include "scene/vulkan/scene_frame.h"
#include "scene/vulkan/scene_targets.h"

namespace bro::scene::vk {

namespace {

void copyWhole(VkCommandBuffer cmd, const SceneVkImage& src, const SceneVkImage& dst, VkImageAspectFlags aspect) {
    VkImageCopy region{};
    region.srcSubresource = {aspect, 0, 0, 1};
    region.dstSubresource = {aspect, 0, 0, 1};
    region.extent = {src.width, src.height, 1};
    vkCmdCopyImage(cmd, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}

}  // namespace

bool PassDepthSnapshot::active(const SceneFrame& frame) const {
    if (frame.ssao || frame.ssr || frame.dof) return true;
    for (const SceneNode* node : frame.lists.nodes) {
        if (node->type() == SceneNode::Type::Decal) return true;
        if (node->type() == SceneNode::Type::Particles3D) {
            const auto* p = static_cast<const Particles3DNode*>(node);
            if (p->softness() > 0.0f && p->liveCount() > 0) return true;
        }
    }
    return false;
}

void PassDepthSnapshot::declare(const SceneFrame& frame, PassIO& io) const {
    io.transferSrc(frame.gpu.targets.depth);
    io.transferDst(frame.gpu.targets.depthSnapshot);
}

void PassDepthSnapshot::record(SceneFrame& frame) {
    copyWhole(frame.cmd, frame.gpu.targets.depth, frame.gpu.targets.depthSnapshot, VK_IMAGE_ASPECT_DEPTH_BIT);
}

bool PassColorSnapshot::active(const SceneFrame& frame) const {
    return frame.ssr;
}

void PassColorSnapshot::declare(const SceneFrame& frame, PassIO& io) const {
    io.transferSrc(frame.gpu.targets.hdr);
    io.transferDst(frame.gpu.targets.ssrSnapshot);
}

void PassColorSnapshot::record(SceneFrame& frame) {
    copyWhole(frame.cmd, frame.gpu.targets.hdr, frame.gpu.targets.ssrSnapshot, VK_IMAGE_ASPECT_COLOR_BIT);
}

}  // namespace bro::scene::vk
