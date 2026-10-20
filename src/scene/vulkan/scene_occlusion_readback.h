#pragma once

// The CPU's copy of the opaque depth's pyramid (SceneTargets::hiZ): each
// frame PassHiZ copies its coarse levels (from the first at most
// kMaxWidth texels wide) into this frame slot's host-visible buffer, and
// the frame that next takes the slot, which VulkanFrames has already
// waited for, reads it into the OcclusionSnapshot (scene/occlusion_snapshot.h)
// with the view it was drawn from. Nothing waits for it: the snapshot is
// kFramesInFlight frames old. It copies only while a user asks for the
// snapshot (snapshot()), so the first ask finds none for a few frames.
//
// Measured with the turf's surface layers (SurfaceLayers' selection treating
// a hidden tile as unseen) on the wilds: the shortfall did not move, being
// the visible tiles', while tiles behind swaying trees came and went (walk:
// fills 3.1 -> 17.6 a frame, 0.1 -> 9.2 tiles a frame coarse), so the
// selection does not use it. Its users are what skip work on what is
// hidden for some frames, as far cards' captures of cells behind terrain.

#include "scene/occlusion_snapshot.h"
#include "scene/vulkan/scene_vk_allocator.h"
#include "render/vulkan_frames.h"

#include <bromath/mat.h>
#include <bromath/vec.h>
#include <vulkan/vulkan.h>

#include <cstdint>

namespace bro::scene::vk {

class SceneOcclusionReadback {
public:
    static constexpr uint32_t kMaxWidth = 160;

    /// Size the slots for a pyramid `width` x `height` at level 0 with
    /// `levels` levels, of a frame `frameWidth` x `frameHeight`; drops the
    /// snapshot and what the slots hold. False on allocation failure.
    bool resize(SceneVkAllocator& allocator, uint32_t width, uint32_t height, uint32_t levels, uint32_t frameWidth,
                uint32_t frameHeight);
    void cleanup(SceneVkAllocator& allocator);

    /// Copy `hiZ` (in GENERAL, written by the compute stage), drawn from
    /// `proj` x `relView` at `eye`, into frame slot `slot` (serial `frame`).
    void record(VkCommandBuffer cmd, VkImage hiZ, uint32_t slot, uint64_t frame, const bromath::Mat4& proj,
                const bromath::Mat4& relView, const bromath::Vec3& eye);
    /// At a frame's start (slot `slot`, serial `frame`, its earlier use
    /// done): the slot's copy, if it holds one from an earlier frame, into
    /// the snapshot.
    void harvest(uint32_t slot, uint64_t frame);

    /// The snapshot, for a user at frame `frame` (its serial): asking keeps
    /// the copies coming, which stop kIdleFrames after the last ask (the
    /// snapshot is then invalid until they start again).
    const OcclusionSnapshot& snapshot(uint64_t frame) {
        askedAt_ = frame;
        return snapshot_;
    }

private:
    static constexpr uint64_t kIdleFrames = 8;
    uint64_t askedAt_ = 0;
    struct Slot {
        SceneVkBuffer buffer;
        uint64_t frame = 0;   // the serial it was written at; 0: nothing
        bromath::Mat4 proj, relView;
        bromath::Vec3 eye{0.0f, 0.0f, 0.0f};
    };
    Slot slots_[render::VulkanFrames::kFramesInFlight];
    uint32_t firstLevel_ = 0;
    uint32_t levels_ = 0;      // copied, from firstLevel_
    uint32_t width_ = 0, height_ = 0;
    uint32_t frameWidth_ = 0, frameHeight_ = 0;
    OcclusionSnapshot snapshot_;
};

}  // namespace bro::scene::vk
