#pragma once

// Exposure on the GPU (scene/exposure.h; docs/lighting-api.js): the scale the
// tonemap multiplies the HDR frame by, fixed or metered.
//
// Each frame, in the frame's graphics stream right before the tonemap
// (PassPostFx), one dispatch (exposure.comp): in the auto mode a histogram
// of the HDR frame's log2 luminance (one pixel of every 4 x 4 block), which
// its last workgroup to finish meters, moving the applied EV toward the
// metered one by the frame's time step and writing the state buffer the
// tonemap reads (binding 3 of its set). One dispatch and one barrier: a
// histogram pass and an adapt pass apart cost a drained GPU between them,
// more than the work. The fixed mode's one workgroup writes the CPU's
// scale, exact. Nothing waits:
// the state lives on the GPU from frame to frame, and the CPU learns the
// applied and metered EV from a copy into a small host ring it reads once
// the frame that wrote it is done (CullStats::exposureEv, meteredEv).

#include "scene/exposure.h"
#include "scene/vulkan/frame/scene_vk_allocator.h"

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>

namespace bro::scene::vk {

struct SceneFrame;
struct SceneGpu;
struct SceneVkImage;

class SceneExposure {
public:
    bool setup(SceneGpu& gpu);
    void cleanup(SceneGpu& gpu);

    /// Meter `hdr` (SHADER_READ_ONLY, outside any rendering) and write this
    /// frame's state into frame.cmd; the frame's stats get the newest values
    /// the GPU finished.
    void record(SceneFrame& frame, const SceneVkImage& hdr);
    /// The state buffer the tonemap reads (16 bytes: ev, metered, scale, adapting).
    VkBuffer state() const { return state_.buffer; }
    static constexpr VkDeviceSize kStateBytes = 16;

    /// The frame recorded last was submitted as `ticket`.
    void submitted(uint64_t ticket);

private:
    struct Push {
        int32_t width, height;
        float minLog2, maxLog2;
        float mode;
        float fixedEv;
        float fixedScale;
        float compensation;
        float dt;
        float speedUp;
        float speedDown;
        float minEv;
        float maxEv;
        float lowPercent;
        float highPercent;
    };
    /// A copy of the state for the CPU, and the submission that wrote it.
    struct Readback {
        SceneVkBuffer buffer;
        uint64_t ticket = 0;
        uint64_t serial = 0;
        bool pending = false;   // recorded, not yet submitted
    };
    static constexpr int kReadbacks = 4;

    void readStats(SceneFrame& frame);

    SceneVkDevice* device_ = nullptr;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    SceneVkBuffer histogram_;   // kBins uints and a done count, cleared by the last workgroup
    SceneVkBuffer state_;
    bool cleared_ = false;      // the buffers zeroed on the GPU
    std::array<Readback, kReadbacks> readbacks_{};
    uint64_t serial_ = 0;
    // The newest values read back.
    float ev_ = 0.0f, metered_ = 0.0f, scale_ = 1.0f;
};

}  // namespace bro::scene::vk
