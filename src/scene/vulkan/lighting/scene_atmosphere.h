#pragma once

// The atmosphere's tables on the GPU (world/atmosphere.h;
// Hillaire 2020; the model and the tables' parametrisations are
// scene/world/atmosphere.h and shaders/common/atmosphere.bk).
//
// View-independent tables, remade only when the air changes (its media,
// the planet, the ground's albedo), as world-service compute jobs
// (scene/world/world_service.h), required, so the frame they change in is
// the frame they are made in:
//  - transmittance (256 x 64, (mu, r)): the air from a point to the top;
//  - multiple scattering (64 x 32, (mu_s, r)): the light scattered twice and
//    more, as an isotropic source per unit of scattering (Hillaire's
//    L2 / (1 - f_ms));
//  - the sky's light (64 x 32 x 5, (mu_s, altitude)): the radiance arriving
//    from every direction as l <= 2 spherical harmonics in the sun's frame
//    (the sky above, the lit ground below), from which every surface reads
//    its ambient with direction (common/lighting.bk, open_sky_irradiance).
// View tables, per frame, one of each per frame slot:
//  - the sky view (256 x 128, (azimuth from the sun, zenith)) at the eye,
//    remade when the eye's height or the sun moved enough to matter (or the
//    tables changed); the sky draws from it. It is made in the graphics
//    stream: it is small and seldom remade, and the sky that reads it then
//    draws before the frame waits for its compute, which keeps the frame's
//    first scope (its clear and the sky) beside the plants' plans;
//  - the aerial perspective (32 x 32 x 32 froxels over the main camera's
//    frustum, slices squared in depth out to the camera's far plane or the
//    air's horizon): the in-scatter and transmittance between the eye and
//    every lit surface (common/lighting.bk, air), in the graphics
//    stream beside the sky view. It is a small dispatch: on the async
//    compute queue it queued behind the plants' plans and the frame's join
//    waited for it, while the graphics idled ahead of the join anyway.
// Every table holds light for a sun of illuminance 1; the block carries the
// sun's (AtmosphereGpu::sunIlluminance), so the sun's colour and strength
// cost nothing.
//
// The sun. The atmosphere's sun is the brightest directional light (the
// light above the air: every lit shader dims it by the transmittance at the
// surface, so the light arriving is what the sky shows), unless the
// AtmosphereParams name its direction or illuminance outright.

#include "scene/vulkan/frame/scene_vk_allocator.h"
#include "scene/vulkan/frame/scene_vk_descriptors.h"
#include "scene/vulkan/world/scene_world_service.h"

#include <bromath/vec.h>
#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <vector>

namespace bro::scene::vk {

struct SceneFrame;
struct SceneGpu;
class SceneDefaults;

/// The tables read back to the CPU (SceneAtmosphere::read), RGBA floats,
/// x fastest: bro_atmosphere_test checks them against a reference.
struct AtmosphereTables {
    AtmosphereGpu block{};              // the last render's (the main view's)
    float froxelInvViewProj[16] = {};   // its froxel camera's clip -> relative position
    std::vector<float> transmittance;   // kTransmittanceW x kTransmittanceH
    std::vector<float> multiScatter;    // kMultiScatterSun x kMultiScatterAlt
    std::vector<float> skyView;         // kSkyViewW x kSkyViewH
    std::vector<float> skyLight;        // kSkyLightSun x kSkyLightAlt x kSkyLightLayers
    std::vector<float> inscatter;       // kFroxelXY^2 x kFroxelSlices
    std::vector<float> airTransmittance;
};

class SceneAtmosphere final : public WorldProducer {
public:
    bool setup(SceneGpu& gpu);
    void cleanup(SceneGpu& gpu);

    /// The frame begins (before its lighting is filled): the sun, the
    /// block for the main view, whether the tables and this slot's sky view
    /// are to be made.
    void beginFrame(SceneFrame& frame);
    /// On this frame.
    bool active() const { return on_; }
    /// This frame's planet radius (m) and the eye about its centre, as the
    /// lighting block has them.
    float planetRadius() const { return block_.shell[0]; }
    void eyeAbout(const bromath::Vec3& eye, float out[4]) const { originFor(eye, out); }
    /// The block for a view whose eye is `eye` (the frame's own, or a
    /// reflection probe's): the frame's with its origin there. Off: zero,
    /// and no light is the sun.
    void fill(AtmosphereGpu& out, const bromath::Vec3& eye) const;
    /// The lighting set's atmosphere bindings (9-13).
    void writeBindings(SceneVkDescriptorWriter& writer) const;
    /// The view-independent tables a compute stage reads (in GENERAL): the
    /// transmittance and the sky's light.
    VkImageView transmittanceView() const { return transmittance_.view; }
    VkImageView skyLightView() const { return skyLight_.view; }

    /// This frame's view tables, the sky view and the froxels, into the
    /// graphics stream (PassAtmosphere).
    void recordView(SceneFrame& frame);
    /// The world service wrote the tables on the async compute queue this
    /// frame: their readers wait for the frame's compute.
    bool tablesOnCompute() const { return tablesOnCompute_; }

    /// The tables of the last render, copied out (waits for the GPU).
    bool read(SceneGpu& gpu, AtmosphereTables& out);

    // WorldProducer: the view-independent tables.
    void offer(SceneFrame& frame, WorldService& service) override;
    double record(SceneFrame& frame, int kind, std::span<const WorldJob> granted, WorldStream& stream) override;

private:
    enum Job : uint32_t { kTransmittance, kMultiScatter, kSkyLight, kSkyView, kAerial, kJobs };
    /// The compute block (lighting/atmosphere_tables.bk's AtmosphereBlock).
    struct Block {
        AtmosphereGpu atm;
        float froxelInvViewProj[16];
        float job[4];
    };
    struct Slot {
        SceneVkImage skyView, inscatter, transmittance;
        bool valid = false;
        bool tables = false;   // made from the tables as they are now (not before they were)
        float r = 0.0f, muS = 0.0f;
        uint64_t version = 0;
    };

    void dispatch(SceneFrame& frame, VkCommandBuffer cmd, Job job, const Slot& slot, float r, float muS);
    /// The block's origin for an eye at `eye`.
    void originFor(const bromath::Vec3& eye, float out[4]) const;
    static void barrier(VkCommandBuffer cmd, bool apart, bool before);

    SceneVkDevice* device_ = nullptr;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    std::array<VkPipeline, kJobs> pipelines_{};
    SceneVkImage transmittance_, multiScatter_, skyLight_;
    std::vector<Slot> slots_;
    int kind_ = -1;

    // This frame.
    bool on_ = false;
    AtmosphereGpu block_{};
    float froxelInv_[16] = {};
    bromath::Vec3 eye_{0.0f, 0.0f, 0.0f};
    double seaLevel_ = 0.0;
    double center_[3] = {0.0, 0.0, 0.0};
    bool centered_ = false;
    uint32_t slot_ = 0;
    bool remakeSkyView_ = false;
    bool tablesOnCompute_ = false;
    // The tables: the parameters' hash wanted, made, and its version.
    uint64_t wantHash_ = 0, madeHash_ = 0;
    uint64_t version_ = 0;
    bool made_ = false;
    bool rendered_ = false;   // a frame ran with the atmosphere on
};

}  // namespace bro::scene::vk
