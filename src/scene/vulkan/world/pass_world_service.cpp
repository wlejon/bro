#include "scene/vulkan/world/pass_world_service.h"

#include "scene/vulkan/frame/scene_frame.h"
#include "scene/vulkan/world/scene_world_service.h"

namespace bro::scene::vk {

void PassWorldService::record(SceneFrame& frame) { frame.gpu.world.runFrame(frame); }

}  // namespace bro::scene::vk
