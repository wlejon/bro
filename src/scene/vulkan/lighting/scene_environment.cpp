#include "scene/vulkan/lighting/scene_environment.h"
#include "scene/vulkan/frame/scene_shader_sets.h"

#include "scene/scene_renderer.h"
#include "scene/vulkan/frame/scene_defaults.h"
#include "scene/vulkan/frame/scene_frame.h"
#include "scene/vulkan/frame/scene_fullscreen.h"
#include "scene/vulkan/frame/scene_vk_device.h"
#include "scene/vulkan/frame/scene_vk_pipeline.h"
#include "scene/vulkan/frame/scene_vk_shader_compiler.h"
#include "util/log.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bro::scene::vk {

namespace {

// The GL renderer's sizes: a 1024² radiance cube matches a 4k panorama's
// angular density; diffuse irradiance is low frequency; six prefilter mips
// step roughness by 0.2.
constexpr uint32_t kRadianceSize = 1024;
constexpr uint32_t kIrradianceSize = 32;
constexpr uint32_t kPrefilterSize = 256;
constexpr uint32_t kPrefilterMips = 6;
constexpr uint32_t kBrdfLutSize = 512;
constexpr VkFormat kBrdfFormat = VK_FORMAT_R16G16_SFLOAT;

enum SkyVariant : uint32_t { kSkyAtmosphere, kSkybox, kSkyStars };

float halfToFloat(uint16_t h) {
    const uint32_t sign = (h >> 15) & 1u, exp = (h >> 10) & 31u, man = h & 1023u;
    float v;
    if (exp == 0) v = std::ldexp(static_cast<float>(man), -24);
    else if (exp == 31) v = man ? 0.0f : INFINITY;
    else v = std::ldexp(static_cast<float>(man | 1024u), static_cast<int>(exp) - 25);
    return sign ? -v : v;
}

// The irradiance over pi on a surface facing +Y from an RGBA16F panorama
// (row 0 the +Y pole, env_convert.frag's), as the irradiance cube holds it:
// (1/pi) * the integral of L cos over the upper hemisphere, sampled on a
// grid of at most 512 x 256 texels. Texels env_convert.frag's sanitize()
// would zero (negative, NaN, infinite) count as it does.
void skyIrradianceUp(const std::vector<uint16_t>& px, uint32_t w, uint32_t h, float out[3]) {
    out[0] = out[1] = out[2] = 0.0f;
    if (w == 0 || h == 0) return;
    const uint32_t sx = std::max(1u, w / 512), sy = std::max(1u, h / 256);
    double sum[3] = {0.0, 0.0, 0.0};
    for (uint32_t y = 0; y < h / 2; y += sy) {
        const double theta = 3.14159265358979 * (0.5 - (y + 0.5 * sy) / h);   // elevation
        const double weight = std::sin(theta) * std::cos(theta) * (3.14159265358979 * sy / h) *
                              (2.0 * 3.14159265358979 * sx / w);
        for (uint32_t x = 0; x < w; x += sx) {
            const uint16_t* p = &px[(static_cast<size_t>(y) * w + x) * 4];
            float c[3] = {halfToFloat(p[0]), halfToFloat(p[1]), halfToFloat(p[2])};
            if (!(std::isfinite(c[0]) && std::isfinite(c[1]) && std::isfinite(c[2]))) continue;
            for (int k = 0; k < 3; ++k) sum[k] += std::max(c[k], 0.0f) * weight;
        }
    }
    for (int k = 0; k < 3; ++k) out[k] = static_cast<float>(sum[k] / 3.14159265358979);
}

uint32_t mipCount(uint32_t size) {
    return static_cast<uint32_t>(std::floor(std::log2(static_cast<float>(size)))) + 1;
}

bool createCube(SceneVkAllocator& allocator, uint32_t size, uint32_t mips, SceneVkImage& out) {
    return allocator.createImage(size, size, SceneEnvironment::kCubeFormat,
                                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, out, mips, VK_SAMPLE_COUNT_1_BIT,
                                 VK_IMAGE_ASPECT_COLOR_BIT, 6, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT);
}

// Every mip and layer of `image` from its current layout to `layout`.
void transitionAll(SceneVkAllocator& allocator, VkCommandBuffer cmd, SceneVkImage& image, VkImageLayout layout) {
    allocator.transitionImageLayout(cmd, image.image, image.format, image.currentLayout, layout, image.mipLevels, 0,
                                    VK_IMAGE_ASPECT_COLOR_BIT, image.arrayLayers, 0);
    image.currentLayout = layout;
}

}  // namespace

bool SceneEnvironment::setup(SceneVkDevice& device, SceneVkAllocator&, const SceneDefaults& defaults) {
    dev_ = device.device();

    bakeSetLayout_ = fullscreen::samplerSetLayout(dev_, 1);
    bakeLayout_ = bakeSetLayout_ ? fullscreen::layout(dev_, bakeSetLayout_, sizeof(BakePush)) : VK_NULL_HANDLE;
    postVs_ = SceneVkShaderCompiler::createBuiltinModule(dev_, BuiltinSceneShader::PostFxVert);
    convertFs_ = SceneVkShaderCompiler::createBuiltinModule(dev_, BuiltinSceneShader::EnvConvertFrag);
    irradianceFs_ = SceneVkShaderCompiler::createBuiltinModule(dev_, BuiltinSceneShader::IrradianceFrag);
    prefilterFs_ = SceneVkShaderCompiler::createBuiltinModule(dev_, BuiltinSceneShader::PrefilterFrag);
    brdfFs_ = SceneVkShaderCompiler::createBuiltinModule(dev_, BuiltinSceneShader::BrdfLutFrag);
    convertPipeline_ = fullscreen::pipeline(dev_, bakeLayout_, postVs_, convertFs_, kCubeFormat);
    irradiancePipeline_ = fullscreen::pipeline(dev_, bakeLayout_, postVs_, irradianceFs_, kCubeFormat);
    prefilterPipeline_ = fullscreen::pipeline(dev_, bakeLayout_, postVs_, prefilterFs_, kCubeFormat);
    brdfPipeline_ = fullscreen::pipeline(dev_, bakeLayout_, postVs_, brdfFs_, kBrdfFormat);

    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = VK_FILTER_LINEAR;
    info.minFilter = VK_FILTER_LINEAR;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.maxAnisotropy = 1.0f;
    if (vkCreateSampler(dev_, &info, nullptr, &equirectSampler_) != VK_SUCCESS) equirectSampler_ = VK_NULL_HANDLE;

    const VkDescriptorSetLayout skySets[2] = {defaults.cameraLayout, defaults.lightingLayout};
    VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 2;
    layoutInfo.pSetLayouts = skySets;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(dev_, &layoutInfo, nullptr, &skyLayout_) != VK_SUCCESS) skyLayout_ = VK_NULL_HANDLE;
    skyVs_ = SceneVkShaderCompiler::createBuiltinModule(dev_, BuiltinSceneShader::SkyVert);
    atmosphereFs_ = SceneVkShaderCompiler::createBuiltinModule(dev_, BuiltinSceneShader::SkyAtmosphereFrag);
    skyboxFs_ = SceneVkShaderCompiler::createBuiltinModule(dev_, BuiltinSceneShader::SkyboxFrag);
    starfieldFs_ = SceneVkShaderCompiler::createBuiltinModule(dev_, BuiltinSceneShader::StarfieldFrag);

    if (!convertPipeline_ || !irradiancePipeline_ || !prefilterPipeline_ || !brdfPipeline_ || !equirectSampler_ ||
        !skyLayout_ || !skyVs_ || !atmosphereFs_ || !skyboxFs_ || !starfieldFs_) {
        LOG_ERROR("SceneEnvironment: Failed creating the environment pipelines");
        return false;
    }
    return true;
}

void SceneEnvironment::cleanup(SceneVkDevice& device, SceneVkAllocator& allocator) {
    release(allocator);
    allocator.destroyImage(brdfLut_);
    skyPipelines_.destroy(device);
    for (VkPipeline* p : {&convertPipeline_, &irradiancePipeline_, &prefilterPipeline_, &brdfPipeline_}) {
        if (*p) destroyScenePipeline(dev_, *p, nullptr);
        *p = VK_NULL_HANDLE;
    }
    for (VkShaderModule* m : {&postVs_, &convertFs_, &irradianceFs_, &prefilterFs_, &brdfFs_, &skyVs_, &atmosphereFs_,
                              &skyboxFs_, &starfieldFs_}) {
        SceneVkShaderModule::destroy(dev_, *m);
        *m = VK_NULL_HANDLE;
    }
    for (VkPipelineLayout* l : {&bakeLayout_, &skyLayout_}) {
        if (*l) vkDestroyPipelineLayout(dev_, *l, nullptr);
        *l = VK_NULL_HANDLE;
    }
    if (bakeSetLayout_) vkDestroyDescriptorSetLayout(dev_, bakeSetLayout_, nullptr);
    bakeSetLayout_ = VK_NULL_HANDLE;
    if (equirectSampler_) vkDestroySampler(dev_, equirectSampler_, nullptr);
    equirectSampler_ = VK_NULL_HANDLE;
}

void SceneEnvironment::release(SceneVkAllocator& allocator) {
    allocator.destroyImage(radiance_);
    allocator.destroyImage(irradiance_);
    allocator.destroyImage(prefiltered_);
}

// --- Bake -------------------------------------------------------------------

void SceneEnvironment::update(SceneGpu& gpu, VkCommandBuffer cmd, SceneRenderer& renderer) {
    // The split sum's BRDF table serves every specular sky, the atmosphere's
    // and the reflection probes' as well as an environment's: it is made
    // whether or not one is loaded.
    ensureBrdfLut(gpu, cmd);
    if (renderer.environmentGeneration() == generation_) return;
    generation_ = renderer.environmentGeneration();
    release(gpu.allocator);
    if (!renderer.hasEnvironment()) return;
    if (!brdfLut_.isValid() || !bake(gpu, cmd, renderer)) {
        LOG_ERROR("SceneEnvironment: baking '%s' failed; the scene keeps its flat ambient",
                  renderer.environmentPath().c_str());
        release(gpu.allocator);
        // Nothing is loaded after all: hasEnvironment() says so from here on.
        renderer.clearEnvironment();
        generation_ = renderer.environmentGeneration();
    }
}

bool SceneEnvironment::ensureBrdfLut(SceneGpu& gpu, VkCommandBuffer cmd) {
    if (brdfLut_.isValid()) return true;
    if (!gpu.allocator.createImage(kBrdfLutSize, kBrdfLutSize, kBrdfFormat,
                                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, brdfLut_)) {
        LOG_ERROR("SceneEnvironment: Failed creating the BRDF LUT");
        return false;
    }
    transitionAll(gpu.allocator, cmd, brdfLut_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    fullscreen::draw(gpu.device, cmd, brdfLut_, brdfPipeline_, bakeLayout_, VK_NULL_HANDLE);
    transitionAll(gpu.allocator, cmd, brdfLut_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return true;
}

bool SceneEnvironment::bake(SceneGpu& gpu, VkCommandBuffer cmd, SceneRenderer& renderer) {
    SceneVkAllocator& alloc = gpu.allocator;
    const std::vector<uint16_t> pixels = renderer.takeEnvironmentPixels();
    const auto width = static_cast<uint32_t>(renderer.environmentWidth());
    const auto height = static_cast<uint32_t>(renderer.environmentHeight());
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(width) * height * 4 * sizeof(uint16_t);
    if (pixels.size() * sizeof(uint16_t) != bytes) {
        LOG_ERROR("SceneEnvironment: the environment's pixels were already taken");
        return false;
    }
    const uint32_t maxDim = gpu.device.context().deviceProperties().limits.maxImageDimension2D;
    if (width > maxDim || height > maxDim) {
        LOG_ERROR("SceneEnvironment: a %ux%u panorama exceeds the device's %u texel limit", width, height, maxDim);
        return false;
    }

    // The panorama goes through a staging buffer of its own: it can be far
    // larger than the frame's upload arena.
    SceneVkBuffer staging;
    if (!alloc.createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging) ||
        !staging.mappedData) {
        alloc.destroyBuffer(staging);
        LOG_ERROR("SceneEnvironment: Failed creating a %llu byte staging buffer",
                  static_cast<unsigned long long>(bytes));
        return false;
    }
    std::memcpy(staging.mappedData, pixels.data(), bytes);
    skyIrradianceUp(pixels, width, height, skyUp_);
    SceneVkImage equirect;
    const bool ok = alloc.createImage(width, height, kCubeFormat,
                                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, equirect) &&
                    createCube(alloc, kRadianceSize, mipCount(kRadianceSize), radiance_) &&
                    createCube(alloc, kIrradianceSize, 1, irradiance_) &&
                    createCube(alloc, kPrefilterSize, kPrefilterMips, prefiltered_);
    if (!ok) {
        alloc.destroyBuffer(staging);
        alloc.destroyImage(equirect);
        LOG_ERROR("SceneEnvironment: Failed creating the environment images");
        return false;
    }

    transitionAll(alloc, cmd, equirect, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {width, height, 1};
    vkCmdCopyBufferToImage(cmd, staging.buffer, equirect.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    transitionAll(alloc, cmd, equirect, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // Panorama -> radiance cube mip 0, then its mip chain (the irradiance and
    // prefilter passes and the Krivanek bias read the blurrier levels).
    transitionAll(alloc, cmd, radiance_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    renderFaces(gpu, cmd, radiance_, 0, convertPipeline_, samplerSet(gpu, equirect.view, equirectSampler_), {});
    transitionAll(alloc, cmd, radiance_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    alloc.generateMipmaps(cmd, radiance_.image, kCubeFormat, kRadianceSize, kRadianceSize, radiance_.mipLevels, 0, 6);
    radiance_.currentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    transitionAll(alloc, cmd, irradiance_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    renderFaces(gpu, cmd, irradiance_, 0, irradiancePipeline_,
                samplerSet(gpu, radiance_.view, gpu.defaults.cubeSampler), {});
    transitionAll(alloc, cmd, irradiance_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    const bool prefilteredOk = prefilter(gpu, cmd, radiance_, prefiltered_);
    alloc.destroyBuffer(staging);
    alloc.destroyImage(equirect);
    if (prefilteredOk) {
        LOG_INFO("Loaded HDR environment '%s' (%ux%u -> cube %u², irradiance %u², prefilter %u² x %u mips)",
                 renderer.environmentPath().c_str(), width, height, kRadianceSize, kIrradianceSize, kPrefilterSize,
                 kPrefilterMips);
    }
    return prefilteredOk;
}

bool SceneEnvironment::prefilter(SceneGpu& gpu, VkCommandBuffer cmd, const SceneVkImage& src, SceneVkImage& dst) {
    if (!src.isValid() || !dst.isValid()) return false;
    VkDescriptorSet set = samplerSet(gpu, src.view, gpu.defaults.cubeSampler);
    transitionAll(gpu.allocator, cmd, dst, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    for (uint32_t mip = 0; mip < dst.mipLevels; ++mip) {
        BakePush push{};
        push.roughness = dst.mipLevels <= 1 ? 0.0f : static_cast<float>(mip) / static_cast<float>(dst.mipLevels - 1);
        push.envSize = static_cast<float>(src.width);
        renderFaces(gpu, cmd, dst, mip, prefilterPipeline_, set, push);
    }
    transitionAll(gpu.allocator, cmd, dst, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return true;
}

VkImageView SceneEnvironment::faceView(SceneGpu& gpu, const SceneVkImage& image, uint32_t face, uint32_t mip) {
    VkImageViewCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info.image = image.image;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = image.format;
    info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, face, 1};
    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(dev_, &info, nullptr, &view) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkDevice dev = dev_;
    gpu.device.defer([dev, view] { vkDestroyImageView(dev, view, nullptr); });
    return view;
}

void SceneEnvironment::renderFaces(SceneGpu& gpu, VkCommandBuffer cmd, SceneVkImage& dst, uint32_t mip,
                                   VkPipeline pipeline, VkDescriptorSet set, BakePush push) {
    const uint32_t size = std::max(1u, dst.width >> mip);
    for (uint32_t face = 0; face < 6; ++face) {
        VkImageView view = faceView(gpu, dst, face, mip);
        if (!view) continue;
        push.face = static_cast<int32_t>(face);
        fullscreen::draw(gpu.device, cmd, view, size, size, pipeline, bakeLayout_, set, &push, sizeof(push));
    }
}

VkDescriptorSet SceneEnvironment::samplerSet(SceneGpu& gpu, VkImageView view, VkSampler sampler) {
    VkDescriptorSet set = gpu.device.frameSet(bakeSetLayout_);
    SceneVkDescriptorWriter writer;
    writer.writeImage(0, view, sampler);
    writer.updateSet(dev_, set);
    return set;
}

// --- Lighting ---------------------------------------------------------------

void SceneEnvironment::fillLighting(SceneLightingUniforms& out, const SceneRenderer& renderer) const {
    const bool ibl = radiance_.isValid() && irradiance_.isValid() && prefiltered_.isValid() && brdfLut_.isValid();
    out.iblParams[0] = ibl ? 1.0f : 0.0f;
    out.iblParams[1] = renderer.environmentIntensity();
    out.iblParams[2] = renderer.environmentRotation();
    out.iblParams[3] = ibl ? static_cast<float>(prefiltered_.mipLevels - 1) : 0.0f;
    // The ground's bounce (SceneRenderer::setGroundBounce), worked out here
    // once a frame: a level ground of that albedo lit by the sun (over pi
    // for the light it sends back) and by the sky over it, as irradiance
    // over pi and the environment's intensity, so scene_lighting.glsl's
    // sceneAmbient adds it times the share of a surface's lower hemisphere
    // that is ground. Needs the sun filled in first (scene_lighting.cpp).
    const float* gb = renderer.groundBounce();
    const float intensity = renderer.environmentIntensity();
    for (int k = 0; k < 4; ++k) out.groundBounce[k] = 0.0f;
    if (ibl && intensity > 1e-4f && gb[0] + gb[1] + gb[2] > 0.0f) {
        float sunUp = 0.0f;
        if (out.sunDirection[3] > 0.5f) {
            const float* d = out.sunDirection;
            const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            sunUp = len > 1e-6f ? std::max(-d[1] / len, 0.0f) : 0.0f;
        }
        for (int k = 0; k < 3; ++k) {
            const float onGround = out.sunColor[k] * out.sunColor[3] * sunUp / 3.14159265f + skyUp_[k] * intensity;
            out.groundBounce[k] = gb[k] * onGround / intensity;
        }
    }
}

void SceneEnvironment::writeBindings(SceneVkDescriptorWriter& writer, const SceneDefaults& d) const {
    auto cube = [&](const SceneVkImage& image) { return image.isValid() ? image.view : d.cube.view; };
    // Sampled images: the set's own sampler (binding 8) filters them.
    auto image = [&](uint32_t binding, VkImageView view) {
        writer.writeImage(binding, view, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
    };
    image(4, cube(irradiance_));
    image(5, cube(prefiltered_));
    image(6, brdfLut_.isValid() ? brdfLut_.view : d.black.view);
    image(7, cube(radiance_));
}

VkImageView SceneEnvironment::prefilteredView(const SceneDefaults& d) const {
    return prefiltered_.isValid() ? prefiltered_.view : d.cube.view;
}

// --- Sky --------------------------------------------------------------------

bool SceneEnvironment::skyVisible(const SceneRenderer& renderer, bool perspective) const {
    return perspective && (renderer.atmosphere().enabled || radiance_.isValid() || renderer.starfield().enabled);
}

void SceneEnvironment::drawSky(SceneGpu& gpu, VkCommandBuffer cmd, const TargetFormat& target,
                               VkDescriptorSet cameraSet, VkDescriptorSet lightingSet, const SceneRenderer& renderer) {
    auto pipeline = [&](SkyVariant variant, VkShaderModule fs) {
        return skyPipelines_.get(variant, target, [&] {
            SceneVkPipelineBuilder b;
            b.setShaderStages(skyVs_, fs)
             .setInputTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
             .setPolygonMode(VK_POLYGON_MODE_FILL)
             .setCullMode(VK_CULL_MODE_NONE)
             .setTarget(target)
             .disableDepthTest();
            if (variant == kSkyStars) {
                // Added onto the sky's colour; the indirect attachment
                // untouched. The stars are sky, so the frame is covered
                // (alpha 1) with or without a background under them.
                VkPipelineColorBlendAttachmentState add{};
                add.blendEnable = VK_TRUE;
                add.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
                add.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
                add.colorBlendOp = VK_BLEND_OP_ADD;
                add.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                add.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
                add.alphaBlendOp = VK_BLEND_OP_ADD;
                add.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
                b.setColorBlendAttachment(0, add);
            } else {
                b.disableBlending(target.colorCount);
            }
            return b.build(gpu.device.device(), skyLayout_);
        });
    };

    const VkDescriptorSet sets[2] = {cameraSet, lightingSet};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skyLayout_, 0, 2, sets, 0, nullptr);
    VkPipeline background = VK_NULL_HANDLE;
    if (renderer.atmosphere().enabled) {
        background = pipeline(kSkyAtmosphere, atmosphereFs_);
    } else if (radiance_.isValid()) {
        background = pipeline(kSkybox, skyboxFs_);
    }
    if (background) {
        bindScenePipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, background);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }
    const StarfieldParams& stars = renderer.starfield();
    if (!stars.enabled) return;
    if (VkPipeline p = pipeline(kSkyStars, starfieldFs_)) {
        const float push[4] = {stars.intensity, stars.density, stars.rotation, 0.0f};
        bindScenePipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
        vkCmdPushConstants(cmd, skyLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), push);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }
}

}  // namespace bro::scene::vk
