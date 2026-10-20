#include "scene/vulkan/pass_ground_layers.h"

#include "scene/vulkan/scene_frame.h"
#include "scene/vulkan/scene_mesh_drawer.h"

namespace bro::scene::vk {

void PassGroundLayers::record(SceneFrame& frame) { frame.gpu.meshes.recordGroundLayers(frame); }

}  // namespace bro::scene::vk
