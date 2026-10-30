#include "scene/vulkan/draw/pass_volume.h"

#include "render/vulkan_context.h"
#include "scene/vulkan/draw/scene_mesh_drawer.h"
#include "scene/vulkan/frame/scene_defaults.h"
#include "scene/vulkan/frame/scene_frame.h"
#include "scene/vulkan/frame/scene_targets.h"
#include "scene/vulkan/frame/scene_vk_device.h"

#include <algorithm>
#include <cstring>

namespace bro::scene::vk {

namespace {
// The volumes the exposure meter reads, the first in draw order.
constexpr uint32_t kMeteredVolumes = 8;
}

bool PassVolume::active(const SceneFrame& frame) const {
    return !frame.lists.volumes.empty();
}

void PassVolume::declare(const SceneFrame& frame, PassIO& io) const {
    io.sample(frame.gpu.targets.depthSnapshot);
    io.hdr();
}

void PassVolume::record(SceneFrame& frame) {
    // The meter's slots in this frame's upload memory, each "no medium" (no
    // light, all the sun) until its volume writes it: a volume whose program
    // failed leaves the meter as it was. Without the memory nothing draws
    // (frameUpload has said so).
    const uint32_t metered = std::min<uint32_t>(static_cast<uint32_t>(frame.lists.volumes.size()), kMeteredVolumes);
    constexpr uint32_t kFloats = SceneFrame::kVolumeMeterFloats;
    const VkDeviceSize bytes = metered * kFloats * sizeof(float);
    const VkDeviceSize align =
        frame.gpu.device.context().deviceProperties().limits.minStorageBufferOffsetAlignment;
    render::UploadSlice meter = frame.gpu.device.frameUpload(bytes, std::max<VkDeviceSize>(align, 16));
    if (!meter) return;
    const float none[kFloats] = {0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f};
    for (uint32_t i = 0; i < metered; ++i)
        std::memcpy(static_cast<float*>(meter.mapped) + kFloats * i, none, sizeof(none));
    frame.volumeMeter = meter.buffer;
    frame.volumeMeterOffset = meter.offset;
    frame.meteredVolumes = metered;

    // The opaque depth in the material set's first slot, the rest its
    // defaults, and the meter's slots.
    const SceneDefaults& d = frame.gpu.defaults;
    const SceneVkImage& depth = frame.gpu.targets.depthSnapshot;
    VkDescriptorSet set = frame.gpu.device.frameSet(d.materialLayout);
    SceneVkDescriptorWriter writer;
    writer.writeImage(0, depth.view, depth.sampler);
    writer.writeImage(1, d.flatNormal.view, d.sampler);
    writer.writeImage(2, d.white.view, d.sampler);
    writer.writeImage(3, d.black.view, d.sampler);
    writer.writeImage(4, d.white.view, d.sampler);
    writer.writeBuffer(5, meter.buffer, bytes, meter.offset, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    writer.updateSet(frame.gpu.device.device(), set);

    uint32_t index = 0;
    for (const MeshDraw& draw : frame.lists.volumes) {
        frame.gpu.meshes.recordVolume(frame.cmd, frame.hdrTarget, frame.cameraSet, frame.lightingSet, set, draw,
                                      index);
        frame.drewContent = true;
        ++index;
    }
}

}  // namespace bro::scene::vk
