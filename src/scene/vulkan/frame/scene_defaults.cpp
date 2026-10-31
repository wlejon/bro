#include "scene/vulkan/frame/scene_defaults.h"

#include "scene/vulkan/draw/scene_vk_custom_shader.h"

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
    repeatSampler = makeSampler(dev, VK_LOD_CLAMP_NONE, VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT);
    mipSampler = makeSampler(dev, VK_LOD_CLAMP_NONE);
    repeatSSampler = makeSampler(dev, VK_LOD_CLAMP_NONE, VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                 VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    if (!sampler || !cubeSampler || !nearestSampler || !repeatSampler || !mipSampler || !repeatSSampler) {
        LOG_ERROR("SceneDefaults: Failed creating the default samplers");
        return false;
    }

    cameraLayout = SceneVkDescriptorLayoutBuilder::createCameraLayout(dev);
    lightingLayout = SceneVkDescriptorLayoutBuilder::createLightingLayout(dev, device.shadowCompareSampler(), cubeSampler);
    SceneVkDescriptorLayoutBuilder material;
    for (uint32_t i = 0; i < 5; ++i)
        material.addBinding(i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
    // A volume's readings for the exposure meter (volume.frag, PassVolume).
    material.addBinding(5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
    materialLayout = material.build(dev);

    SceneVkDescriptorLayoutBuilder bone;
    bone.addBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, kVsFs);
    // A procedural draw's segment records.
    bone.addBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kVsFs);
    boneLayout = bone.build(dev);

    SceneVkDescriptorLayoutBuilder custom;
    custom.addBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, kVsFs);
    for (uint32_t i = 1; i <= kCustomImages; ++i)
        custom.addBinding(i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kVsFs);
    // The images' samplers, in CustomShaderInterface::SamplerMode order.
    using CS = CustomShaderInterface;
    static_assert(CS::kLinear == 0 && CS::kRepeat == 1 && CS::kRepeatS == 2 && CS::kNearest == 3 &&
                  CS::kSamplerModes == 4 && CS::kSamplersBinding == kCustomImages + 1);
    custom.addSamplers(CS::kSamplersBinding, {mipSampler, repeatSampler, repeatSSampler, nearestSampler}, kVsFs);
    // A brokernel hook's (custom.bk): the same four, one a binding, then its
    // storage buffers.
    static_assert(CS::kKernelSamplersBinding == CS::kSamplersBinding + 1);
    const VkSampler kernelSamplers[CS::kSamplerModes] = {mipSampler, repeatSampler, repeatSSampler, nearestSampler};
    for (uint32_t i = 0; i < CS::kSamplerModes; ++i)
        custom.addSamplers(CS::kKernelSamplersBinding + i, {kernelSamplers[i]}, kVsFs);
    for (uint32_t i = 0; i < CS::kMaxBuffers; ++i)
        custom.addBinding(CS::kBuffersBinding + i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kVsFs);
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
    if (!allocator.createImage(1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, zeroVolume, 1, VK_SAMPLE_COUNT_1_BIT,
                               VK_IMAGE_ASPECT_COLOR_BIT, 1, 0, VK_IMAGE_VIEW_TYPE_3D, 1)) {
        LOG_ERROR("SceneDefaults: Failed creating the fallback 3D image");
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
    allocator.destroyImage(zeroVolume);
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
