#include "scene/vulkan/draw/pass_volume.h"

#include "scene/vulkan/draw/scene_mesh_drawer.h"
#include "scene/vulkan/frame/scene_defaults.h"
#include "scene/vulkan/frame/scene_frame.h"
#include "scene/vulkan/frame/scene_targets.h"
#include "scene/vulkan/frame/scene_vk_device.h"

namespace bro::scene::vk {

bool PassVolume::active(const SceneFrame& frame) const {
    return !frame.lists.volumes.empty();
}

void PassVolume::declare(const SceneFrame& frame, PassIO& io) const {
    io.sample(frame.gpu.targets.depthSnapshot);
    io.hdr();
}

void PassVolume::record(SceneFrame& frame) {
    // The opaque depth in the material set's first slot, the rest its defaults.
    const SceneDefaults& d = frame.gpu.defaults;
    const SceneVkImage& depth = frame.gpu.targets.depthSnapshot;
    VkDescriptorSet set = frame.gpu.device.frameSet(d.materialLayout);
    SceneVkDescriptorWriter writer;
    writer.writeImage(0, depth.view, depth.sampler);
    writer.writeImage(1, d.flatNormal.view, d.sampler);
    writer.writeImage(2, d.white.view, d.sampler);
    writer.writeImage(3, d.black.view, d.sampler);
    writer.writeImage(4, d.white.view, d.sampler);
    writer.updateSet(frame.gpu.device.device(), set);

    for (const MeshDraw& draw : frame.lists.volumes) {
        frame.gpu.meshes.recordVolume(frame.cmd, frame.hdrTarget, frame.cameraSet, frame.lightingSet, set, draw);
        frame.drewContent = true;
    }
}

}  // namespace bro::scene::vk
