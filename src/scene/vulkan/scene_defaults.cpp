#include "scene/vulkan/scene_defaults.h"

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

VkSampler makeSampler(VkDevice dev, float maxLod) {
    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = VK_FILTER_LINEAR;
    info.minFilter = VK_FILTER_LINEAR;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
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

    cameraLayout = SceneVkDescriptorLayoutBuilder::createCameraLayout(dev);
    lightingLayout = SceneVkDescriptorLayoutBuilder::createLightingLayout(dev, device.shadowCompareSampler());
    materialLayout = SceneVkDescriptorLayoutBuilder::createMaterialLayout(dev);

    SceneVkDescriptorLayoutBuilder bone;
    bone.addBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT);
    boneLayout = bone.build(dev);

    SceneVkDescriptorLayoutBuilder custom;
    custom.addBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, kVsFs);
    for (uint32_t i = 1; i <= 8; ++i)
        custom.addBinding(i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, kVsFs);
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

    sampler = makeSampler(dev, 0.0f);
    cubeSampler = makeSampler(dev, 16.0f);
    if (!sampler || !cubeSampler) {
        LOG_ERROR("SceneDefaults: Failed creating the default samplers");
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
    for (VkSampler* s : {&sampler, &cubeSampler}) {
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
