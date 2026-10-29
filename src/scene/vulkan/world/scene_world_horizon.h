#pragma once
// The land's horizon about an eye (shaders/world/world_horizon_layout.h): for each direction about the eye and
// each distance, the steepest slope of the land out to there. The cell pass
// (scene_world_cells_pass.h) leaves out of the camera's view the cells under
// it, so an eye-level view plans and draws none of the forest the hills
// hide.
//
// The land's producer makes it on the graphics queue, as a world service job
// after the land's fills (scene_mesh_drawer_land.cpp), whenever the eye has
// moved kRemakeMetres from the last one's or the land's tiles changed; the
// cell pass runs on the compute queue ahead of the frame's graphics, so it
// reads the newest one the graphics queue has finished (a frame or two old),
// and none once the eye is kDriftMetres from where that one was made
// (world_horizon_layout.h's WH_MARGIN allows for that much). Two buffers:
// the one a frame's compute reads is never the one its graphics writes.

#include "scene/vulkan/frame/scene_vk_allocator.h"

#include <bromath/vec.h>
#include <vulkan/vulkan.h>

#include <cstdint>

namespace bro::render {
class VulkanQueue;
}

namespace bro::scene::vk {

class WorldHorizon {
public:
    static constexpr float kRemakeMetres = 0.5f;
    static constexpr float kDriftMetres = 2.0f;

    /// A horizon to read: its buffer (WH_WORDS floats) and the graphics
    /// ticket the reader's compute waits for.
    struct Read {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceSize size = 0;
        uint64_t ticket = 0;
    };

    /// The newest horizon `graphics` has finished, made within kDriftMetres
    /// of `eye`; null when there is none. This frame's writes go elsewhere.
    const Read* read(const render::VulkanQueue& graphics, const bromath::Vec3& eye);
    /// Whether this frame should make one about `eye`: none made yet, the
    /// last made kRemakeMetres from it, or `changed` (the land's tiles).
    bool stale(const bromath::Vec3& eye, bool changed) const;
    /// The buffer to make this frame's into about `eye` (made on first
    /// use, shared with the compute queue); null when it cannot be made.
    const SceneVkBuffer* write(SceneVkAllocator& allocator, const bromath::Vec3& eye);
    /// The frame went to the graphics queue as `ticket`: what it wrote is
    /// read once that completes.
    void submitted(uint64_t ticket);
    /// No land: nothing made so far is read again.
    void forget();
    void destroy(SceneVkAllocator& allocator);

private:
    struct Slot {
        SceneVkBuffer buffer;
        bromath::Vec3 eye{0.0f, 0.0f, 0.0f};
        uint64_t ticket = 0;    // the frame that wrote it (0: none, or not yet submitted)
        bool pending = false;   // written this frame
    };
    Slot slots_[2];
    int newest_ = -1;     // the slot last written
    int reading_ = -1;    // the slot this frame's read() handed out
    Read read_;
};

}  // namespace bro::scene::vk
