#include "scene/vulkan/ground/pass_ground_layers.h"

#include "scene/vulkan/frame/scene_frame.h"
#include "scene/vulkan/draw/scene_mesh_drawer.h"

namespace bro::scene::vk {

void PassGroundLayers::record(SceneFrame& frame) { frame.gpu.meshes.recordGroundLayers(frame); }

}  // namespace bro::scene::vk
