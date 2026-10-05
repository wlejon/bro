#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>
#include <vector>

namespace bro::scene::vk {

/// The camera block (set 0), std140 — shaders/scene_camera.glsl. The
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
    alignas(16) float windParams[4]; // x = time (s), y = frequency, z = the eye's phase offset
};

/// The light and shadow-tile capacity of the lighting block (SceneRenderer's
/// light list and shadow plan never exceed them).
constexpr int kSceneMaxLights = 32;
constexpr int kSceneMaxShadowTiles = 16;

/// One light — SceneLight in shaders/scene_lighting.glsl.
struct alignas(16) SceneLightUniform {
    alignas(16) float position[4];      // xyz = camera-relative position, w = type (0 dir, 1 point, 2 spot)
    alignas(16) float direction[4];     // xyz = unit direction (light -> scene), w = range
    alignas(16) float color[4];         // rgb = color, a = intensity
    alignas(16) float shadow[4];        // x = cos(inner), y = cos(outer), z = first tile (-1), w = tile count
    alignas(16) float cascadeSplit[4];  // cascade far view distances
};

/// One shadow atlas tile — ShadowTile in shaders/scene_lighting.glsl.
struct alignas(16) SceneShadowTileUniform {
    alignas(16) float matrix[16];       // camera-relative position -> (tile uv, [0,1] depth)
    alignas(16) float rect[4];          // atlas uv origin.xy, size.zw
    alignas(16) float bias[4];          // const depth bias, normal offset, texel world const, per metre
    alignas(16) float depth[4];         // near, far, ortho
};

/// The lighting block (set 1), std140 — shaders/scene_lighting.glsl.
struct alignas(16) SceneLightingUniforms {
    alignas(16) float sunDirection[4];  // dominant directional light, w = 1 when present
    alignas(16) float sunColor[4];      // rgb = color, a = intensity
    alignas(16) float ambientColor[4];  // rgb = flat ambient
    alignas(16) float params[4];        // light count, PCF taps, 1/atlas size, shadow tile count
    SceneLightUniform lights[kSceneMaxLights];
    SceneShadowTileUniform shadows[kSceneMaxShadowTiles];
    // The probe and shade map, like every position here, camera-relative.
    alignas(16) float probeWorldToLocal[16];
    alignas(16) float probeLocalToWorld[16];
    alignas(16) float probePos[4];      // xyz = pos, w = enabled (1 or 0)
    alignas(16) float probeBoxSize[4];  // xyz = box size, w = boxProjection (1 or 0)
    alignas(16) float probeParams[4];   // x = intensity, y = blendDist, z = maxLOD, w = pad
    alignas(16) float shadeOrigin[4];   // xyz = origin, w = hasShadeMap (1 or 0)
    alignas(16) float shadeParams[4];   // x = cellSize, y = hex, z = width, w = height
    alignas(16) float iblParams[4];     // x = enabled, y = intensity, z = rotation (rad), w = prefilter max LOD
    // The atmosphere (shaders/scene_atmosphere.glsl); atmSunDir.w = enabled.
    alignas(16) float atmSunDir[4];     // xyz = unit vector towards the sun
    alignas(16) float atmSunColor[4];   // rgb = solar irradiance, a = planet radius
    alignas(16) float atmBetaR[4];      // rgb = Rayleigh scattering per metre, a = thickness
    alignas(16) float atmParams[4];     // x = Mie scattering, y = Mie g, z = Rayleigh / w = Mie scale height
    alignas(16) float atmParams2[4];    // x = sea level, y = spherical, z = multi-scatter, w = sun angular radius
    alignas(16) float atmCenter[4];     // xyz = planet centre, w = sun disk intensity
};
static_assert(sizeof(SceneLightUniform) == 80 && sizeof(SceneShadowTileUniform) == 112);

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

    /// Set 1 of every lit pipeline: lighting UBO, shadow atlas (with
    /// `shadowCompareSampler` immutable — SceneVkDevice::shadowCompareSampler),
    /// reflection probe cubemap, tile shade map, then the environment's
    /// irradiance and prefiltered cubes, the BRDF LUT and the sky cube.
    static VkDescriptorSetLayout createLightingLayout(VkDevice device, VkSampler shadowCompareSampler,
                                                      VkSampler linearSampler);

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
