#pragma once

#include "scene/vulkan/shaders/ground/land_sun_layout.h"
#include "scene/vulkan/shaders/lighting/probe_grid_layout.h"
#include "scene/vulkan/shaders/shadow/shadow_pages_layout.h"

#include <vulkan/vulkan.h>
#include <cstdint>
#include <vector>

namespace bro::scene::vk {

/// The camera block (set 0), std140 — shaders/common/scene_camera.glsl. The
/// matrices are camera-relative (scene_view.h): view takes (world - eye).
struct alignas(16) SceneCameraUniforms {
    alignas(16) float view[16];
    alignas(16) float proj[16];
    alignas(16) float viewProj[16];
    alignas(16) float invView[16];
    alignas(16) float invProj[16];
    alignas(16) float eyeWorld[4];   // xyz = the eye's absolute world position
    alignas(16) float viewport[4];   // x = width, y = height, z = near, w = far
    alignas(16) float fogParams[4];  // x = start, y = end, z = density, w = start distance
    alignas(16) float fogColor[4];   // rgb = color, a = heightFalloff
    alignas(16) float wind[4];       // xyz = direction, w = strength
    alignas(16) float windParams[4]; // x = time (s), y = frequency, z = the eye's phase offset, w = the oscillator clock (time mod kWindClockPeriod)
    alignas(16) float windField[4];  // xy = horizontal direction (x, z), z = steady speed, w = gust travel speed
    alignas(16) float windGust[4];   // x = gust amount, y = 1 / along scale, z = 1 / across scale
    // The plant movers (plant_movers.h, scene_vegetation.glsl's vegMoverPush).
    alignas(16) float plantMoverInfo[4];   // x = movers, y = trail samples a mover
    // Two a mover: (camera-relative x, z now, the reach of its trail's push
    // (m), its radius), (height, strength, 0, 0).
    alignas(16) float plantMovers[8][4];
    // kTrail a mover, oldest first: (camera-relative x, z, age (s), window).
    alignas(16) float plantTrail[80][4];
};

/// The light and shadow-map capacity of the lighting block (SceneRenderer's
/// light list and shadow plan never exceed them).
constexpr int kSceneMaxLights = 32;
constexpr int kSceneMaxShadowMaps = SP_MAX_MAPS;

/// One light — SceneLight in shaders/common/scene_lighting.glsl.
struct alignas(16) SceneLightUniform {
    alignas(16) float position[4];      // xyz = camera-relative position, w = type (0 dir, 1 point, 2 spot)
    alignas(16) float direction[4];     // xyz = unit direction (light -> scene), w = range
    alignas(16) float color[4];         // rgb = color, a = intensity
    alignas(16) float shadow[4];        // x = cos(inner), y = cos(outer), z = first shadow map (-1), w = map count
    alignas(16) float shadowReach[4];   // x = how far from the eye it shadows (0: no limit)
};

/// One virtual shadow map (a sun's clipmap level, a spot's map, a point
/// light's face) — ShadowMapGpu in shaders/common/scene_shadow_pages.glsl.
struct alignas(16) SceneShadowMapUniform {
    alignas(16) float matrix[16];       // camera-relative position -> (map uv, [0,1] depth)
    alignas(16) float info[4];          // first page-table entry, kind (SP_KIND_*), window origin x, y (mod SP_WINDOW)
    alignas(16) float bias[4];          // const depth bias, normal offset, texel (m, a clipmap level's), per metre (a local map's finest)
    alignas(16) float depth[4];         // near, far, ortho, clipmap level
};

/// The marking pass's block (std140) — shaders/shadow/shadow_mark.comp.
struct alignas(16) SceneShadowMarkUniforms {
    alignas(16) float invViewProj[16];  // Vulkan clip (the camera's reversed depth) -> camera-relative
    alignas(16) float grid[4];          // x, y = the depth target's size, z = pixels a side an invocation marks for, w = lights
    alignas(16) float view[4];          // x = metres a pixel a metre (perspective), y = metres a pixel (ortho), z = finest texel, w = levels
    alignas(16) float soft[4];          // x = the sun's search level (-1: none), its lighting's shadowSoft.z
    alignas(16) float lights[kSceneMaxLights][4];    // first map, maps, kind (0 sun, 1 spot, 2 point), reach (0: none)
    alignas(16) float lightPos[kSceneMaxLights][4];  // camera-relative position, range
    // The sampled maps, then the sun's next clipmap's levels while it is built.
    SceneShadowMapUniform maps[SP_MARK_MAPS];
};

/// The atmosphere's block (std140) — AtmosphereGpu in
/// shaders/common/scene_atmosphere_params.glsl; filled by SceneAtmosphere::fill
/// (scene/vulkan/lighting/scene_atmosphere.h). Lengths in metres, positions
/// about the planet's centre.
struct alignas(16) AtmosphereGpu {
    alignas(16) float sunDir[4];          // xyz = towards the sun, w = 1 when the atmosphere is on
    alignas(16) float sunIlluminance[4];  // rgb = at the top of the air, w = the sun's angular radius
    alignas(16) float rayleigh[4];        // rgb = scattering per metre at the ground, w = scale height
    alignas(16) float mie[4];             // x = scattering, y = absorption, z = g, w = scale height
    alignas(16) float ozone[4];           // rgb = absorption at the peak, w = the peak's altitude
    alignas(16) float shell[4];           // x = planet radius, y = top radius, z = ozone half width, w = spherical
    alignas(16) float origin[4];          // xyz = the eye about the planet's centre
    alignas(16) float ground[4];          // rgb = ground albedo, w = lights[] index of the sun (-1: none)
    alignas(16) float skyView[4];         // x = r, y = mu_s the sky-view table was made at, z = jitter,
                                          // w = the froxels' share of the air's lookups
    alignas(16) float froxel[4];          // xyz = this eye minus the froxel eye, w = the volume's depth
    alignas(16) float froxelViewProj[16]; // relative to the froxel eye -> its clip space
};
static_assert(sizeof(AtmosphereGpu) == 10 * 16 + 64);

/// The probe grid's block (std140) — ProbeGridGpu in
/// shaders/lighting/scene_probe_grid.glsl; filled by SceneProbeGrid::fill
/// (scene/vulkan/lighting/scene_probe_grid.h). Positions relative to the eye.
struct alignas(16) ProbeGridGpu {
    alignas(16) float params[4];                     // x on, y cascades, z the layers' top (m), w the lowest layer (m)
    alignas(16) float eye[4];                        // xyz the eye in the world, w the layers' power
    alignas(16) float sun[4];                        // xyz toward the sun
    alignas(16) float cascade[PG_MAX_CASCADES][4];   // xy its world's corner (x, z), z spacing, w 1/spacing
    alignas(16) float wrap[PG_MAX_CASCADES][4];      // xy where column 0 is stored
};
static_assert(sizeof(ProbeGridGpu) == 176);

/// The land's sun's block (std140) — LandSunGpu in
/// shaders/common/scene_lighting.glsl; filled by SceneMeshDrawer::fillLandSun
/// (scene/world/land_sun.h). Positions relative to the eye.
struct alignas(16) LandSunGpu {
    alignas(16) float params[4];                 // x 1 when any level is shown, y the eye's world height
    alignas(16) float body[4];                   // a round body's: xy the eye's world x and z, z 1 / (2 R); 0 flat
    alignas(16) float level[LS_LEVELS][4];       // xy its corner (x, z), z its texel (m), w the layer shown (-1: none)
};
static_assert(sizeof(LandSunGpu) == 16 * (2 + LS_LEVELS));

/// The cloud deck's shadow's block (std140) — CloudShadowGpu in
/// shaders/common/scene_lighting.glsl; filled by SceneClouds::fill
/// (vulkan/sky/scene_clouds.h). Directions in air space.
struct alignas(16) CloudShadowGpu {
    alignas(16) float params[4];   // x 1 when shown, y the datum's radius, z the finest level's texel (m), w growth
    alignas(16) float n[4];        // the levels' plane's normal
    alignas(16) float e1[4];       // its axes
    alignas(16) float e2[4];
    alignas(16) float toBody[3][4];   // the clouds' turn into the body's frame (CloudsUBO's toBody)
    alignas(16) float drift[4];       // xyz what the wind has carried the forms (m, the body's frame), w a cloud's edge (m)
    alignas(16) float shell[4];       // x the deck's bottom radius, y its top's
};
static_assert(sizeof(CloudShadowGpu) == 144);

/// The lighting block (set 1), std140 — shaders/common/scene_lighting.glsl.
struct alignas(16) SceneLightingUniforms {
    alignas(16) float sunDirection[4];  // dominant directional light, w = its index + 1 (0: none)
    alignas(16) float sunColor[4];      // rgb = color, a = intensity
    alignas(16) float ambientColor[4];  // rgb = flat ambient
    alignas(16) float params[4];        // light count, PCF taps, 1 / the shadow pool's side (texels), pool columns
    alignas(16) float shadowView[4];    // x = metres a pixel a metre (perspective), y = metres a pixel (ortho), z = finest texel, w = the sway level (-1: no overlays this frame)
    alignas(16) float shadowSoft[4];    // x = the sun's penumbra a metre along the light, y = the still penumbra (m), z = the search level (-1: none)
    SceneLightUniform lights[kSceneMaxLights];
    SceneShadowMapUniform shadowMaps[kSceneMaxShadowMaps];
    // The probe and shade map, like every position here, camera-relative.
    alignas(16) float probeWorldToLocal[16];
    alignas(16) float probeLocalToWorld[16];
    alignas(16) float probePos[4];      // xyz = pos, w = enabled (1 or 0)
    alignas(16) float probeBoxSize[4];  // xyz = box size, w = boxProjection (1 or 0)
    alignas(16) float probeParams[4];   // x = intensity, y = blendDist, z = maxLOD, w = pad
    alignas(16) float shadeOrigin[4];   // xyz = origin, w = hasShadeMap (1 or 0)
    alignas(16) float shadeParams[4];   // x = cellSize, y = hex, z = width, w = height
    alignas(16) float iblParams[4];     // x = enabled, y = intensity, z = rotation (rad), w = prefilter max LOD
    alignas(16) float groundBounce[4];  // rgb = the ground's bounce, irradiance over pi and intensity (0: none)
    AtmosphereGpu atm;                  // the atmosphere (sunDir[3] = 1 when on)
    ProbeGridGpu probeGrid;             // the probe grid (params[0] = 1 when on)
    LandSunGpu landSun;                 // the land's sun (params[0] = 1 when shown)
    CloudShadowGpu cloudShadow;         // the cloud deck's shadow (params[0] = 1 when shown)
};
static_assert(sizeof(SceneLightUniform) == 80 && sizeof(SceneShadowMapUniform) == 112);

/// Utility for constructing VkDescriptorSetLayouts with arbitrary bindings.
class SceneVkDescriptorLayoutBuilder {
public:
    /// `immutableSampler` (count 1 only) bakes the sampler into the layout;
    /// writes to that binding then supply only the image view.
    void addBinding(uint32_t binding, VkDescriptorType type, uint32_t count, VkShaderStageFlags stageFlags,
                    VkSampler immutableSampler = VK_NULL_HANDLE);
    void clear();

    VkDescriptorSetLayout build(VkDevice device);

    // Standard preset layout builders for the 3D scene engine
    static VkDescriptorSetLayout createCameraLayout(VkDevice device);
    static VkDescriptorSetLayout createMaterialLayout(VkDevice device, uint32_t samplerCount = 5);

    /// Set 1 of every lit pipeline: lighting UBO, the shadow pages' pool
    /// (with `shadowCompareSampler` immutable — SceneVkDevice::
    /// shadowCompareSampler), reflection probe cubemap, tile shade map, then
    /// the environment's irradiance and prefiltered cubes, the BRDF LUT and
    /// the sky cube, the shared sampler (8), the atmosphere's tables (9-13)
    /// and the shadow pages' page table (14), and the probe grid's (17 its
    /// sampler, repeating across; 18-20 its SH, 21 the ground's height, 22
    /// the sun's volume), the land's sun's levels (23) and the cloud
    /// deck's shadow's levels (24).
    static VkDescriptorSetLayout createLightingLayout(VkDevice device, VkSampler shadowCompareSampler,
                                                      VkSampler linearSampler, VkSampler repeatSampler);

private:
    std::vector<VkDescriptorSetLayoutBinding> bindings_;
    std::vector<VkSampler> immutableSamplers_;  // parallel to bindings_
};

/// Helper for recording and dispatching VkWriteDescriptorSet updates.
class SceneVkDescriptorWriter {
public:
    void writeBuffer(uint32_t binding, VkBuffer buffer, VkDeviceSize size,
                     VkDeviceSize offset = 0,
                     VkDescriptorType type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);

    void writeImage(uint32_t binding, VkImageView imageView, VkSampler sampler,
                    VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VkDescriptorType type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);

    void clear();
    void updateSet(VkDevice device, VkDescriptorSet set);

private:
    std::vector<VkDescriptorBufferInfo> bufferInfos_;
    std::vector<VkDescriptorImageInfo> imageInfos_;
    std::vector<VkWriteDescriptorSet> writes_;
};

/// A fixed descriptor pool for sets that are written once and never change
/// while in use (default material sets). Per-frame sets come from
/// SceneVkDevice::frameSet().
class SceneVkDescriptorPool {
public:
    SceneVkDescriptorPool() = default;
    ~SceneVkDescriptorPool();

    bool init(VkDevice device, uint32_t maxSets = 256,
              const std::vector<VkDescriptorPoolSize>& customSizes = {});
    void destroy();

    VkDescriptorSet allocate(VkDescriptorSetLayout layout);
    void reset();

    VkDescriptorPool handle() const { return pool_; }
    VkDevice device() const { return device_; }

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
};

} // namespace bro::scene::vk
