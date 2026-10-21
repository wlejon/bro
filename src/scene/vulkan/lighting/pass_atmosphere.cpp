#include "scene/vulkan/lighting/pass_atmosphere.h"

#include "scene/vulkan/frame/scene_frame.h"
#include "scene/vulkan/lighting/scene_atmosphere.h"

namespace bro::scene::vk {

bool PassAtmosphere::active(const SceneFrame& frame) const { return frame.gpu.atmosphere.active(); }

void PassAtmosphere::declare(const SceneFrame& frame, PassIO& io) const {
    // The sky view, made in the graphics stream, reads the view-independent
    // tables: on a frame the world service remade them on the compute
    // queue, it waits for them.
    if (frame.gpu.atmosphere.tablesOnCompute()) io.readsCompute();
}

void PassAtmosphere::record(SceneFrame& frame) { frame.gpu.atmosphere.recordView(frame); }

}  // namespace bro::scene::vk
