#pragma once

// The probe grid's GPU half (docs/scene-world-architecture.md §7, "Design:
// the probe grid"; the CPU half, where the cascades lie and what they need,
// is scene/world/probe_grid.h; the numbers both and the shaders agree on
// are shaders/lighting/probe_grid_layout.h). On when the renderer's setting
// is (SceneRenderer::setProbeGrid).
//
// The world the probes trace, made a box of a cascade's columns at a time
// (a world-service job, required, raster: in the frame's graphics stream at
// its head, so what reads it this frame reads it made), the boxes the CPU
// half says are stale: the strips a move brought in, the columns a change
// of the canopy (a plants node's records) touched or shades, every column
// when the sun turns. A dispatch a box and stage:
//   - the ground's height: the casters that are opaque and stand still
//     (static and instanced meshes) drawn straight down by the caster drawer
//     (scene_caster_drawer.h) into a depth target, both faces, then made
//     into world heights (probe_height.comp);
//   - the canopy: every plants node's crowns splatted into fixed-point sums
//     (probe_splat.comp) and resolved into the medium, extinction and
//     albedo (probe_resolve.comp);
//   - the sun's transmittance through it and past the ground, and the
//     land's shade alone (probe_sun.comp).
//
// Its probes (l <= 1 SH of the radiance, three 3D images, one a colour
// channel; probe_grid_layout.h), traced through that world as a second
// kind of job, also raster (after the world's, in the same stream): the
// boxes of columns the CPU half offers (fresh where a move brought columns
// in, refresh slices by age), their probes listed for the frame, each
// traced (probe_trace.comp) into a buffer and then blended into the images
// (probe_blend.comp), so a trace reads only what the probes held before it.

#include "scene/vulkan/frame/scene_vk_allocator.h"
#include "scene/vulkan/frame/scene_vk_descriptors.h"
#include "scene/vulkan/shaders/lighting/probe_grid_layout.h"
#include "scene/vulkan/world/scene_world_service.h"
#include "scene/world/probe_grid.h"

#include <bromath/vec.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace bro::scene {
class PlantsNode;
}

namespace bro::scene::vk {

struct SceneFrame;
struct SceneGpu;

/// The probe grid's world read back to the CPU (SceneProbeGrid::read):
/// bro_probe_grid_gpu_test checks it against the scene it was made from.
struct ProbeGridTables {
    ProbeGridGpu block{};         // the last render's
    float captureRange = 0.0f;    // the capture's depth: this far above and below the eye
    // The world, unwrapped: each cascade's from its corner.
    std::vector<float> height;    // PG_HEIGHT x PG_HEIGHT * PG_MAX_CASCADES world heights
    std::vector<float> medium;    // PG_VOXELS^2 x PG_LAYERS_Y * PG_MAX_CASCADES, RGBA: extinction, albedo
    std::vector<float> sun;       // the same voxels' transmittance
    std::vector<float> land;      // and what the land alone leaves of the sun
    // The probes' SH, red, green, blue: PG_COLUMNS^2 x PG_LAYERS *
    // PG_MAX_CASCADES texels of (c0, cx, cy, cz), as stored (wrapped).
    std::vector<float> sh[3];
};

class SceneProbeGrid final : public WorldProducer {
public:
    bool setup(SceneGpu& gpu);
    void cleanup(SceneGpu& gpu);

    /// The frame begins, its draw lists built (before its lighting sets are
    /// written): the canopy gathered, the sun, the cascades placed about the
    /// eye and the block made.
    void beginFrame(SceneFrame& frame);
    /// On this frame (the renderer's setting).
    bool active() const { return on_; }
    /// The block for a view whose eye is `eye` (the frame's, or a
    /// reflection probe's): the frame's with its positions relative to it.
    /// Off: zero.
    void fill(ProbeGridGpu& out, const bromath::Vec3& eye) const;
    /// The lighting set's probe grid bindings (18-20 the SH, 21 the ground's
    /// height, 22 the sun's volume).
    void writeBindings(SceneVkDescriptorWriter& writer) const;

    /// The last render's world, copied out (waits for the GPU).
    bool read(SceneGpu& gpu, ProbeGridTables& out);

    // WorldProducer: the cascades' worlds, and their probes' traces.
    void offer(SceneFrame& frame, WorldService& service) override;
    double record(SceneFrame& frame, int kind, std::span<const WorldJob> granted, WorldStream& stream) override;

    /// The capture's depth reaches this far above and below the eye (m).
    static constexpr float kCaptureRange = 20000.0f;

private:
    enum Stage : uint32_t { kHeight, kSplat, kResolve, kSun, kTrace, kBlend, kStages };
    /// scene_probe_grid_compute.glsl's block.
    struct Block {
        ProbeGridGpu grid;
        float capture[4];
        float sunLight[4];      // rgb the sun's illuminance, w 1 with a sun
        float sky[4];           // rgb the flat ambient, w 0 flat, 1 the environment, 2 the atmosphere
        float environment[4];   // x intensity, y rotation, z the prefiltered level read
        float ground[4];        // rgb the ground's albedo, w the frame's seed
        AtmosphereGpu atm;
    };
    /// What a dispatch makes and reads besides the grid's own: a world
    /// stage's box, a plants node's records (the splat), and its list
    /// (binding 15): the plants it splats, or the probes it traces and blends.
    struct Inputs {
        ProbeBox box;
        VkBuffer plants = VK_NULL_HANDLE;
        VkDeviceSize plantsBytes = 0;
        uint32_t groups = 0;
        uint32_t probes = 0;
        VkDescriptorBufferInfo list{};
    };

    uint32_t recordWorld(SceneFrame& frame, WorldStream& stream);
    double recordTraces(SceneFrame& frame, std::span<const WorldJob> granted, WorldStream& stream);

    /// The canopy this frame: its plants nodes, the cascades their changes
    /// since the last frame fall in told, and the layers' top (above the
    /// tallest plant).
    void gatherCanopy(SceneFrame& frame);
    void capture(SceneFrame& frame, VkCommandBuffer cmd, const std::vector<ProbeBox>& boxes);
    void dispatch(SceneFrame& frame, VkCommandBuffer cmd, Stage stage, const Inputs& in);
    static void computeBarrier(VkCommandBuffer cmd);

    SceneVkDevice* device_ = nullptr;
    VkSampler linear_ = VK_NULL_HANDLE, nearest_ = VK_NULL_HANDLE;
    VkSampler repeat_ = VK_NULL_HANDLE;   // SceneDefaults'
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipelines_[kStages] = {};
    SceneVkImage captureDepth_, height_, medium_, sun_;
    SceneVkImage sh_[3];
    SceneVkBuffer sums_, traces_;
    int kind_ = -1, traceKind_ = -1;

    ProbeGrid grid_;
    bool on_ = false;
    bool rendered_ = false;
    bool shCleared_ = false;
    bool sumsCleared_ = false;   // (then each resolve zeroes what it read)
    double clock_ = -1.0;   // the scene's clock at the last frame (s)
    uint32_t seed_ = 0;
    ProbeGridGpu block_{};
    VkDescriptorBufferInfo blockInfo_{};
    // The canopy's plants nodes this frame, and the generation of each
    // its changes were last taken from.
    struct Canopy {
        const PlantsNode* node = nullptr;
        VkBuffer records = VK_NULL_HANDLE;
        VkDeviceSize bytes = 0;
    };
    std::vector<Canopy> canopy_;
    std::unordered_map<uint32_t, uint64_t> canopySeen_;
    float top_ = 0.0f;
    bromath::Vec3 sunRef_{0.0f, 1.0f, 0.0f};
    uint64_t sunGeneration_ = 1;
};

}  // namespace bro::scene::vk
