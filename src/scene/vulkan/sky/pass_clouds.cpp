#include "scene/vulkan/sky/pass_clouds.h"

#include "scene/vulkan/frame/scene_frame.h"
#include "scene/vulkan/frame/scene_targets.h"
#include "scene/vulkan/sky/scene_clouds.h"

namespace bro::scene::vk {

bool PassCloudsPrepare::active(const SceneFrame& frame) const { return frame.gpu.clouds.active(); }

void PassCloudsPrepare::record(SceneFrame& frame) { frame.gpu.clouds.recordPrepare(frame); }

bool PassClouds::active(const SceneFrame& frame) const { return frame.gpu.clouds.active(); }

void PassClouds::declare(const SceneFrame& frame, PassIO& io) const {
    io.sample(frame.gpu.targets.depthSnapshot);
    io.hdr();
}

void PassClouds::record(SceneFrame& frame) { frame.gpu.clouds.recordDraw(frame); }

}  // namespace bro::scene::vk
