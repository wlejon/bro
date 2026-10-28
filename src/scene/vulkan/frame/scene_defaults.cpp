#include "scene/vulkan/frame/scene_defaults.h"

#include "scene/nodes/mesh_node.h"
#include "scene/vulkan/shaders/ground/scene_land_layout.h"

#include "util/log.h"

namespace bro::scene::vk {

namespace {

bool makePixel(SceneVkAllocator& allocator, const uint8_t (&rgba)[4], SceneVkImage& out) {
    TextureDesc desc{};
    desc.width = 1;
    desc.height = 1;
    desc.format = VK_FORMAT_R8G8B8A8_UNORM;
    desc.generateMipmaps = false;
    desc.enableAnisotropy = false;
    return allocator.createTexture2D(rgba, desc, out);
}

VkSampler makeSampler(VkDevice dev, float maxLod, VkFilter filter = VK_FILTER_LINEAR,
                      VkSamplerAddressMode address = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                      VkSamplerAddressMode addressT = VK_SAMPLER_ADDRESS_MODE_MAX_ENUM) {
    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = filter;
    info.minFilter = filter;
    info.mipmapMode = filter == VK_FILTER_LINEAR ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = address;
    info.addressModeV = addressT == VK_SAMPLER_ADDRESS_MODE_MAX_ENUM ? address : addressT;
    info.addressModeW = address;
    info.maxAnisotropy = 1.0f;
    info.maxLod = maxLod;
    VkSampler s = VK_NULL_HANDLE;
    if (vkCreateSampler(dev, &info, nullptr, &s) != VK_SUCCESS) return VK_NULL_HANDLE;
    return s;
}

}  // namespace

bool SceneDefaults::setup(SceneVkDevice& device, SceneVkAllocator& allocator) {
    VkDevice dev = device.device();
    constexpr VkShaderStageFlags kVsFs = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    sampler = makeSampler(dev, 0.0f);
    cubeSampler = makeSampler(dev, 16.0f);
    nearestSampler = makeSampler(dev, 0.0f, VK_FILTER_NEAREST);
    // (Its mips for the clouds' noise, lighting binding 25; the probe grid's
    // images through it have none.)
    repeatSampler = makeSampler(dev, VK_LOD_CLAMP_NONE, VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT);
    mipSampler = makeSampler(dev, VK_LOD_CLAMP_NONE);
    repeatSSampler = makeSampler(dev, VK_LOD_CLAMP_NONE, VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                 VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    if (!sampler || !cubeSampler || !nearestSampler || !repeatSampler || !mipSampler || !repeatSSampler) {
        LOG_ERROR("SceneDefaults: Failed creating the default samplers");
        return false;
    }

    cameraLayout = SceneVkDescriptorLayoutBuilder::createCameraLayout(dev);
    lightingLayout = SceneVkDescriptorLayoutBuilder::createLightingLayout(dev, device.shadowCompareSampler(), cubeSampler,
                                                                          repeatSampler);
    materialLayout = SceneVkDescriptorLayoutBuilder::createMaterialLayout(dev);

    SceneVkDescriptorLayoutBuilder bone;
    // (A land's LandDraw, which its fragments read too: mesh.frag's
    // SCENE_LAND build.)
    bone.addBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, kVsFs);
    // A procedural draw's segment records, or the plants' records; the
    // plants' leaf pose pass (plants_pose.comp) reads them too, and their
    // fragments (a species' crown and its module's data).
    constexpr VkShaderStageFlags kVsCs = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
    bone.addBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kVsCs | VK_SHADER_STAGE_FRAGMENT_BIT);
    // The plants' chunks this frame, written by their plan.
    bone.addBinding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kVsCs);
    // The plants' leaf poses this frame, written by the pose pass.
    bone.addBinding(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kVsCs);
    // The land's tiles, heights, surface, page table and canopy (binding 0
    // its LandDraw; ground/scene_land.glsl), read by its rings and its
    // fragments: bare images, through the two samplers after them.
    for (uint32_t b = 4; b <= 7; ++b)
        bone.addBinding(b, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kVsFs);
    bone.addBinding(LD_B_LINEAR, VK_DESCRIPTOR_TYPE_SAMPLER, 1, kVsFs, sampler);
    bone.addBinding(LD_B_NEAREST, VK_DESCRIPTOR_TYPE_SAMPLER, 1, kVsFs, nearestSampler);
    boneLayout = bone.build(dev);

    // (A grass node's set is read by the cover's plan too, cover_plan.comp.)
    SceneVkDescriptorLayoutBuilder custom;
    constexpr VkShaderStageFlags kVsFsCs = kVsFs | VK_SHADER_STAGE_COMPUTE_BIT;
    custom.addBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, kVsFsCs);
    for (uint32_t i = 1; i <= static_cast<uint32_t>(MeshNode::kMaxUserTextures); ++i)
        custom.addBinding(i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kVsFsCs);
    // Past them, the land's tiles a grass node stands on (its surface, page
    // table and canopy; its heights and cover take two of the node's own
    // slots).
    static_assert(LG_SURFACES == MeshNode::kMaxUserTextures + 1 && LG_PAGE == LG_SURFACES + 1 &&
                  LG_CANOPY == kCustomBindings && CS_IMAGES == kCustomBindings);
    for (uint32_t i = LG_SURFACES; i <= kCustomBindings; ++i)
        custom.addBinding(i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kVsFsCs);
    // The images' samplers (scene_land_layout.h's CS_*), in CS_* order.
    static_assert(CS_LINEAR == 0 && CS_REPEAT == 1 && CS_REPEAT_S == 2 && CS_NEAREST == 3 && CS_SAMPLER_COUNT == 4);
    custom.addSamplers(CS_SAMPLERS, {mipSampler, repeatSampler, repeatSSampler, nearestSampler}, kVsFsCs);
    customLayout = custom.build(dev);

    SceneVkDescriptorLayoutBuilder empty;
    emptyLayout = empty.build(dev);

    if (!cameraLayout || !lightingLayout || !materialLayout || !boneLayout || !customLayout || !emptyLayout) {
        LOG_ERROR("SceneDefaults: Failed creating the shared set layouts");
        return false;
    }

    if (!makePixel(allocator, {255, 255, 255, 255}, white) ||
        !makePixel(allocator, {128, 128, 255, 255}, flatNormal) ||
        !makePixel(allocator, {0, 0, 0, 255}, black)) {
        LOG_ERROR("SceneDefaults: Failed creating the fallback textures");
        return false;
    }
    if (!allocator.createImage(1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, cube, 1, VK_SAMPLE_COUNT_1_BIT,
                               VK_IMAGE_ASPECT_COLOR_BIT, 6, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT)) {
        LOG_ERROR("SceneDefaults: Failed creating the fallback cube map");
        return false;
    }
    if (!allocator.createImage(1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, zeroArray, 1, VK_SAMPLE_COUNT_1_BIT,
                               VK_IMAGE_ASPECT_COLOR_BIT, 1, 0, VK_IMAGE_VIEW_TYPE_2D_ARRAY)) {
        LOG_ERROR("SceneDefaults: Failed creating the fallback 2D array");
        return false;
    }
    if (!allocator.createImage(1, 1, VK_FORMAT_R32_UINT, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, zeroUintArray, 1, VK_SAMPLE_COUNT_1_BIT,
                               VK_IMAGE_ASPECT_COLOR_BIT, 1, 0, VK_IMAGE_VIEW_TYPE_2D_ARRAY)) {
        LOG_ERROR("SceneDefaults: Failed creating the fallback uint 2D array");
        return false;
    }

    if (!pool_.init(dev, 1)) return false;
    defaultMaterialSet = pool_.allocate(materialLayout);
    if (!defaultMaterialSet) return false;
    SceneVkDescriptorWriter writer;
    writer.writeImage(0, white.view, sampler);
    writer.writeImage(1, flatNormal.view, sampler);
    writer.writeImage(2, white.view, sampler);
    writer.writeImage(3, black.view, sampler);
    writer.writeImage(4, white.view, sampler);
    writer.updateSet(dev, defaultMaterialSet);
    return true;
}

void SceneDefaults::cleanup(SceneVkDevice& device, SceneVkAllocator& allocator) {
    VkDevice dev = device.device();
    pool_.destroy();
    defaultMaterialSet = VK_NULL_HANDLE;
    allocator.destroyImage(white);
    allocator.destroyImage(flatNormal);
    allocator.destroyImage(black);
    allocator.destroyImage(cube);
    allocator.destroyImage(zeroArray);
    allocator.destroyImage(zeroUintArray);
    for (VkSampler* s : {&sampler, &cubeSampler, &nearestSampler, &repeatSampler, &mipSampler, &repeatSSampler}) {
        if (*s != VK_NULL_HANDLE) vkDestroySampler(dev, *s, nullptr);
        *s = VK_NULL_HANDLE;
    }
    for (VkDescriptorSetLayout* l : {&cameraLayout, &lightingLayout, &materialLayout, &boneLayout,
                                     &customLayout, &emptyLayout}) {
        if (*l != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(dev, *l, nullptr);
        *l = VK_NULL_HANDLE;
    }
}

}  // namespace bro::scene::vk
