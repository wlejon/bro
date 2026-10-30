#include "scene/vulkan/lighting/pass_environment.h"

#include "scene/vulkan/lighting/scene_atmosphere.h"
#include "scene/vulkan/lighting/scene_environment.h"
#include "scene/vulkan/frame/scene_frame.h"

namespace bro::scene::vk {

bool PassEnvironment::active(const SceneFrame& frame) const {
    // (A debug view shows what answers it over black.)
    return frame.renderer.debugView() == 0 && frame.gpu.environment.skyVisible(frame.renderer, frame.view.perspective);
}

void PassEnvironment::declare(const SceneFrame& frame, PassIO& io) const {
    io.hdr({.indirect = frame.ssao});
    // The atmosphere's sky reads its sky view (made in the graphics stream)
    // and the sky's light, which waits for the compute on a frame the world
    // service remade it there.
    if (frame.gpu.atmosphere.active() && frame.gpu.atmosphere.tablesOnCompute()) io.readsCompute();
}

void PassEnvironment::record(SceneFrame& frame) {
    frame.gpu.environment.drawSky(frame.gpu, frame.cmd, frame.hdrTarget, frame.cameraSet, frame.lightingSet,
                                  frame.renderer);
}

}  // namespace bro::scene::vk
