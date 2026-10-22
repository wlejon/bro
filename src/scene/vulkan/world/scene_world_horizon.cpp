#include "scene/vulkan/world/scene_world_horizon.h"

#include "render/vulkan_queue.h"
#include "scene/vulkan/shaders/world/world_horizon_layout.h"

#include <cmath>

namespace bro::scene::vk {

namespace {

float metresApart(const bromath::Vec3& a, const bromath::Vec3& b) {
    const bromath::Vec3 d = a - b;
    return std::sqrt(bromath::vdot(d, d));
}

}  // namespace

const WorldHorizon::Read* WorldHorizon::read(const render::VulkanQueue& graphics, const bromath::Vec3& eye) {
    // The newest finished: the last written, else the other.
    reading_ = -1;
    for (const int s : {newest_, newest_ ^ 1}) {
        if (s < 0 || !slots_[s].buffer.isValid() || slots_[s].ticket == 0 || !graphics.isComplete(slots_[s].ticket))
            continue;
        if (metresApart(slots_[s].eye, eye) > kDriftMetres) return nullptr;
        reading_ = s;
        read_ = {slots_[s].buffer.buffer, slots_[s].buffer.size, slots_[s].ticket};
        return &read_;
    }
    return nullptr;
}

bool WorldHorizon::stale(const bromath::Vec3& eye, bool changed) const {
    return changed || newest_ < 0 || metresApart(slots_[newest_].eye, eye) > kRemakeMetres;
}

const SceneVkBuffer* WorldHorizon::write(SceneVkAllocator& allocator, const bromath::Vec3& eye) {
    // Not the slot this frame's compute reads; else the older.
    const int s = reading_ >= 0 ? reading_ ^ 1 : (newest_ < 0 ? 0 : newest_ ^ 1);
    Slot& slot = slots_[s];
    if (!slot.buffer.isValid() &&
        !allocator.createBuffer(VkDeviceSize{WH_WORDS} * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, slot.buffer, /*shared=*/true))
        return nullptr;
    slot.eye = eye;
    slot.ticket = 0;
    slot.pending = true;
    newest_ = s;
    return &slot.buffer;
}

void WorldHorizon::submitted(uint64_t ticket) {
    for (Slot& s : slots_)
        if (s.pending) {
            s.ticket = ticket;
            s.pending = false;
        }
    reading_ = -1;
}

void WorldHorizon::forget() {
    for (Slot& s : slots_) s.ticket = 0;
    newest_ = -1;
}

void WorldHorizon::destroy(SceneVkAllocator& allocator) {
    for (Slot& s : slots_)
        if (s.buffer.isValid()) allocator.destroyBuffer(s.buffer);
    *this = WorldHorizon{};
}

}  // namespace bro::scene::vk
