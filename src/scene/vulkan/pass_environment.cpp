#include "scene/vulkan/pass_environment.h"

#include "scene/vulkan/scene_environment.h"
#include "scene/vulkan/scene_frame.h"

namespace bro::scene::vk {

bool PassEnvironment::active(const SceneFrame& frame) const {
    return frame.gpu.environment.skyVisible(frame.renderer, frame.view.perspective);
}

void PassEnvironment::declare(const SceneFrame& frame, PassIO& io) const {
    io.hdr({.indirect = frame.ssao});
}

void PassEnvironment::record(SceneFrame& frame) {
    frame.gpu.environment.drawSky(frame.gpu, frame.cmd, frame.hdrTarget, frame.cameraSet, frame.lightingSet,
                                  frame.renderer);
}

}  // namespace bro::scene::vk
