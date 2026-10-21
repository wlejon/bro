#pragma once

// The probe grid's GPU half (docs/scene-world-architecture.md §7, "Design:
// the probe grid"; the CPU half, where the cascades lie and what they need,
// is scene/world/probe_grid.h; the numbers both and the shaders agree on
// are shaders/lighting/probe_grid_layout.h). On when the renderer's setting
// is (SceneRenderer::setProbeGrid).
//
// The world the probes trace, made for each cascade where it stands (a
// world-service job, required, raster: in the frame's graphics stream at its
// head, so what reads it this frame reads it made), when it moves, when the
// canopy changes (a plants node's records) or when the sun turns:
//   - the ground's height: the casters that are opaque and stand still
//     (static and instanced meshes) drawn straight down by the caster drawer
//     (scene_caster_drawer.h) into a depth target, both faces, then made
//     into world heights (probe_height.comp);
//   - the canopy: every plants node's crowns splatted into fixed-point sums
//     (probe_splat.comp) and resolved into the medium, extinction and
//     albedo (probe_resolve.comp);
//   - the sun's transmittance through it and past the ground
//     (probe_sun.comp).

#include "scene/vulkan/frame/scene_vk_allocator.h"
#include "scene/vulkan/shaders/lighting/probe_grid_layout.h"
#include "scene/vulkan/world/scene_world_service.h"
#include "scene/world/probe_grid.h"

#include <bromath/vec.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace bro::scene::vk {

struct SceneFrame;
struct SceneGpu;

/// scene_probe_grid.glsl's ProbeGridGpu (std140).
struct ProbeGridGpu {
    float params[4];                       // x on, y cascades, z the layers' top (m), w the lowest layer (m)
    float eye[4];                          // xyz the eye in the world, w the layers' power
    float sun[4];                          // xyz toward the sun
    float cascade[PG_MAX_CASCADES][4];     // xy its world's corner relative to the eye (x, z), z spacing, w 1/spacing
    float wrap[PG_MAX_CASCADES][4];        // xy where column 0 is stored
};
static_assert(sizeof(ProbeGridGpu) == 176);

/// The probe grid's world read back to the CPU (SceneProbeGrid::read):
/// bro_probe_grid_gpu_test checks it against the scene it was made from.
struct ProbeGridTables {
    ProbeGridGpu block{};         // the last render's
    float captureRange = 0.0f;    // the capture's depth: this far above and below the eye
    std::vector<float> height;    // PG_HEIGHT x PG_HEIGHT * PG_MAX_CASCADES world heights
    std::vector<float> medium;    // PG_VOXELS^2 x PG_LAYERS_Y * PG_MAX_CASCADES, RGBA: extinction, albedo
    std::vector<float> sun;       // the same voxels' transmittance
};

class SceneProbeGrid final : public WorldProducer {
public:
    bool setup(SceneGpu& gpu);
    void cleanup(SceneGpu& gpu);

    /// On this frame (the renderer's setting, the images had).
    bool active() const { return on_; }
    const ProbeGridGpu& block() const { return block_; }

    /// The last render's world, copied out (waits for the GPU).
    bool read(SceneGpu& gpu, ProbeGridTables& out);

    // WorldProducer: the cascades' worlds.
    void offer(SceneFrame& frame, WorldService& service) override;
    uint32_t record(SceneFrame& frame, int kind, std::span<const WorldJob> granted, WorldStream& stream) override;

    /// The capture's depth reaches this far above and below the eye (m).
    static constexpr float kCaptureRange = 20000.0f;

private:
    enum Stage : uint32_t { kHeight, kSplat, kResolve, kSun, kStages };
    struct Block {
        ProbeGridGpu grid;
        float capture[4];
    };

    /// The canopy this frame: its plants nodes, a hash of their records'
    /// generations, and the layers' top (above the tallest plant).
    void gatherCanopy(SceneFrame& frame);
    void capture(SceneFrame& frame, VkCommandBuffer cmd, uint32_t mask);
    void dispatch(SceneFrame& frame, VkCommandBuffer cmd, Stage stage, uint32_t mask, VkBuffer plants,
                  VkDeviceSize plantsBytes, uint32_t groups);
    static void computeBarrier(VkCommandBuffer cmd);

    SceneVkDevice* device_ = nullptr;
    VkSampler linear_ = VK_NULL_HANDLE, nearest_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipelines_[kStages] = {};
    SceneVkImage captureDepth_, height_, medium_, sun_;
    SceneVkBuffer sums_;
    int kind_ = -1;

    ProbeGrid grid_;
    bool on_ = false;
    bool rendered_ = false;
    ProbeGridGpu block_{};
    VkDescriptorBufferInfo blockInfo_{};
    // The canopy's plants nodes this frame, and what the volumes were made for.
    struct Canopy {
        uint32_t id = 0;
        VkBuffer records = VK_NULL_HANDLE;
        VkDeviceSize bytes = 0;
        uint32_t plants = 0;
    };
    std::vector<Canopy> canopy_;
    uint64_t canopyHash_ = 0, canopyGeneration_ = 1;
    float top_ = 0.0f;
    bromath::Vec3 sunRef_{0.0f, 1.0f, 0.0f};
    uint64_t sunGeneration_ = 1;
};

}  // namespace bro::scene::vk
