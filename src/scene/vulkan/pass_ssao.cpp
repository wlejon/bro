#include "scene/vulkan/pass_ssao.h"

#include "scene/scene_renderer.h"
#include "scene/vulkan/scene_defaults.h"
#include "scene/vulkan/scene_frame.h"
#include "scene/vulkan/scene_frame_graph.h"
#include "scene/vulkan/scene_fullscreen.h"
#include "scene/vulkan/scene_targets.h"
#include "scene/vulkan/scene_vk_device.h"
#include "scene/vulkan/scene_vk_pipeline.h"
#include "scene/vulkan/scene_vk_shader_compiler.h"
#include "util/log.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bro::scene::vk {

namespace {
constexpr VkFormat kAoFormat = VK_FORMAT_R8_UNORM;
}  // namespace

// --- PassSSAO -----------------------------------------------------------------

namespace {

// The GL renderer's deterministic LCG, so the kernel and noise (and with them
// the AO) are the same across runs, machines and backends. [0, 1).
struct Lcg {
    uint32_t seed = 0x9e3779b9u;
    float operator()() {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) * (1.0f / 16777216.0f);
    }
};

}  // namespace

// Hemisphere kernel: unit vectors with z >= 0 (tangent space, +z along the
// surface normal), each at a random length pulled toward the origin
// (lerp(0.1, 1, t^2)) so samples cluster near the fragment. Then the 4x4
// tiling rotation noise: a random xy per texel in [0, 1], which the shader
// expands to [-1, 1]. One sequence feeds both, as in GL.
void PassSSAO::generateKernel() {
    Lcg frand;
    for (int i = 0; i < 16; ++i) {
        float x = frand() * 2.0f - 1.0f;
        float y = frand() * 2.0f - 1.0f;
        float z = frand();
        float len = std::sqrt(x * x + y * y + z * z);
        if (len < 1e-4f) { x = 0; y = 0; z = 1; len = 1; }
        const float t = static_cast<float>(i) / 16.0f;
        const float scale = (0.1f + 0.9f * t * t) * frand();
        kernel_[i * 4 + 0] = (x / len) * scale;
        kernel_[i * 4 + 1] = (y / len) * scale;
        kernel_[i * 4 + 2] = (z / len) * scale;
        kernel_[i * 4 + 3] = 0.0f;
    }
    for (int i = 0; i < 16; ++i) {
        noisePixels_[i * 4 + 0] = static_cast<uint8_t>(frand() * 255.0f);
        noisePixels_[i * 4 + 1] = static_cast<uint8_t>(frand() * 255.0f);
        noisePixels_[i * 4 + 2] = 0;
        noisePixels_[i * 4 + 3] = 255;
    }
}

bool PassSSAO::createNoise(SceneGpu& gpu) {
    TextureDesc desc{};
    desc.width = 4;
    desc.height = 4;
    desc.magFilter = VK_FILTER_NEAREST;
    desc.minFilter = VK_FILTER_NEAREST;
    desc.generateMipmaps = false;
    desc.enableAnisotropy = false;
    return gpu.allocator.createTexture2D(noisePixels_, desc, noise_);
}

bool PassSSAO::setup(SceneGpu& gpu) {
    device_ = &gpu.device;
    VkDevice dev = gpu.device.device();
    generateKernel();
    if (!createNoise(gpu)) return false;

    VkSamplerCreateInfo sampler{};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = VK_FILTER_LINEAR;
    sampler.minFilter = VK_FILTER_LINEAR;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(dev, &sampler, nullptr, &clampSampler_) != VK_SUCCESS) return false;

    SceneVkDescriptorLayoutBuilder ssaoSet;
    ssaoSet.addBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
    ssaoSet.addBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
    ssaoSet.addBinding(2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
    ssaoSetLayout_ = ssaoSet.build(dev);
    SceneVkDescriptorLayoutBuilder blurSet;
    blurSet.addBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
    blurSetLayout_ = blurSet.build(dev);
    if (!ssaoSetLayout_ || !blurSetLayout_) return false;
    ssaoLayout_ = fullscreen::layout(dev, ssaoSetLayout_, 0);
    blurLayout_ = fullscreen::layout(dev, blurSetLayout_, 4 * sizeof(float));
    if (!ssaoLayout_ || !blurLayout_) return false;

    VkShaderModule vs = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::PostFxVert);
    VkShaderModule ssaoFs = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::SsaoFrag);
    VkShaderModule blurFs = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::BlurFrag);
    ssaoPipeline_ = fullscreen::pipeline(dev, ssaoLayout_, vs, ssaoFs, kAoFormat);
    blurPipeline_ = fullscreen::pipeline(dev, blurLayout_, vs, blurFs, kAoFormat);
    for (VkShaderModule m : {vs, ssaoFs, blurFs}) SceneVkShaderModule::destroy(dev, m);
    return ssaoPipeline_ && blurPipeline_;
}

void PassSSAO::resize(SceneGpu& gpu, uint32_t width, uint32_t height) {
    for (SceneVkImage& img : ao_) gpu.allocator.destroyImage(img);
    const uint32_t w = std::max(1u, width / 2);
    const uint32_t h = std::max(1u, height / 2);
    for (SceneVkImage& img : ao_) {
        if (!gpu.allocator.createImage(w, h, kAoFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, img)) {
            LOG_ERROR("PassSSAO: Failed creating the %ux%u AO targets", w, h);
        }
    }
}

void PassSSAO::cleanup(SceneGpu& gpu) {
    VkDevice dev = gpu.device.device();
    for (SceneVkImage& img : ao_) gpu.allocator.destroyImage(img);
    gpu.allocator.destroyImage(noise_);
    gpu.device.defer([dev, s = clampSampler_, p0 = ssaoPipeline_, p1 = blurPipeline_] {
        if (s) vkDestroySampler(dev, s, nullptr);
        if (p0) vkDestroyPipeline(dev, p0, nullptr);
        if (p1) vkDestroyPipeline(dev, p1, nullptr);
    });
    clampSampler_ = VK_NULL_HANDLE;
    ssaoPipeline_ = blurPipeline_ = VK_NULL_HANDLE;
    for (VkPipelineLayout* l : {&ssaoLayout_, &blurLayout_}) {
        if (*l) vkDestroyPipelineLayout(dev, *l, nullptr);
        *l = VK_NULL_HANDLE;
    }
    for (VkDescriptorSetLayout* l : {&ssaoSetLayout_, &blurSetLayout_}) {
        if (*l) vkDestroyDescriptorSetLayout(dev, *l, nullptr);
        *l = VK_NULL_HANDLE;
    }
}

bool PassSSAO::active(const SceneFrame& frame) const {
    return frame.ssao && ao_[0].isValid() && ao_[1].isValid();
}

void PassSSAO::declare(const SceneFrame& frame, PassIO& io) const {
    io.sample(frame.gpu.targets.depthSnapshot);
}

void PassSSAO::draw(SceneFrame& frame, SceneVkImage& target, VkPipeline pipeline, VkPipelineLayout layout,
                    VkDescriptorSet set, const float* push) {
    SceneFrameGraph::transition(frame.cmd, target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    fullscreen::draw(*device_, frame.cmd, target, pipeline, layout, set, push, push ? 4 * sizeof(float) : 0);
    SceneFrameGraph::transition(frame.cmd, target, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void PassSSAO::record(SceneFrame& frame) {
    const SceneRenderer& r = frame.renderer;
    Uniforms u{};
    std::memcpy(u.proj, frame.view.proj.data, sizeof(u.proj));
    std::memcpy(u.invProj, frame.view.invProj.data, sizeof(u.invProj));
    std::memcpy(u.kernel, kernel_, sizeof(u.kernel));
    u.params[0] = r.ssaoRadius();
    u.params[1] = r.ssaoBias();
    u.params[2] = static_cast<float>(ao_[0].width) / 4.0f;
    u.params[3] = static_cast<float>(ao_[0].height) / 4.0f;
    const VkDescriptorBufferInfo ubo = device_->frameUniform(&u, sizeof(u));

    // Estimate into ao_[0], blur across into ao_[1] and down back into ao_[0].
    VkDescriptorSet ssaoSet = device_->frameSet(ssaoSetLayout_);
    SceneVkDescriptorWriter writer;
    writer.writeImage(0, frame.gpu.targets.depthSnapshot.view, frame.gpu.targets.depthSnapshot.sampler);
    writer.writeImage(1, noise_.view, noise_.sampler);
    writer.writeBuffer(2, ubo.buffer, ubo.range, ubo.offset);
    writer.updateSet(device_->device(), ssaoSet);
    draw(frame, ao_[0], ssaoPipeline_, ssaoLayout_, ssaoSet, nullptr);

    const float across[4] = {1.0f / static_cast<float>(ao_[0].width), 0.0f, 0.0f, 0.0f};
    const float down[4] = {0.0f, 1.0f / static_cast<float>(ao_[0].height), 0.0f, 0.0f};
    for (int pass = 0; pass < 2; ++pass) {
        SceneVkImage& src = ao_[pass];
        SceneVkImage& dst = ao_[1 - pass];
        VkDescriptorSet set = device_->frameSet(blurSetLayout_);
        SceneVkDescriptorWriter blurWriter;
        blurWriter.writeImage(0, src.view, clampSampler_);
        blurWriter.updateSet(device_->device(), set);
        draw(frame, dst, blurPipeline_, blurLayout_, set, pass == 0 ? across : down);
    }
}

// --- PassAoApply ---------------------------------------------------------------

bool PassAoApply::setup(SceneGpu& gpu) {
    device_ = &gpu.device;
    VkDevice dev = gpu.device.device();
    SceneVkDescriptorLayoutBuilder builder;
    builder.addBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
    builder.addBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
    setLayout_ = builder.build(dev);
    if (!setLayout_) return false;
    layout_ = fullscreen::layout(dev, setLayout_, 4 * sizeof(float));
    vs_ = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::PostFxVert);
    fs_ = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::ApplyAoFrag);
    return layout_ && vs_ && fs_;
}

void PassAoApply::cleanup(SceneGpu& gpu) {
    VkDevice dev = gpu.device.device();
    pipelines_.destroy(gpu.device);
    SceneVkShaderModule::destroy(dev, vs_);
    SceneVkShaderModule::destroy(dev, fs_);
    vs_ = fs_ = VK_NULL_HANDLE;
    if (layout_) vkDestroyPipelineLayout(dev, layout_, nullptr);
    if (setLayout_) vkDestroyDescriptorSetLayout(dev, setLayout_, nullptr);
    layout_ = VK_NULL_HANDLE;
    setLayout_ = VK_NULL_HANDLE;
}

bool PassAoApply::active(const SceneFrame& frame) const {
    return ssao_.active(frame) && frame.renderer.ssaoIntensity() > 0.0f;
}

void PassAoApply::declare(const SceneFrame& frame, PassIO& io) const {
    io.sample(frame.gpu.targets.indirect);
    io.hdr();
}

void PassAoApply::record(SceneFrame& frame) {
    VkPipeline pipeline = pipelines_.get(0, frame.hdrTarget, [&] {
        // colour - indirect * (1 - visibility); alpha kept.
        VkPipelineColorBlendAttachmentState blend{};
        blend.blendEnable = VK_TRUE;
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.colorBlendOp = VK_BLEND_OP_REVERSE_SUBTRACT;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.alphaBlendOp = VK_BLEND_OP_ADD;
        blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                               VK_COLOR_COMPONENT_A_BIT;
        SceneVkPipelineBuilder b;
        b.setShaderStages(vs_, fs_)
         .setCullMode(VK_CULL_MODE_NONE)
         .setTarget(frame.hdrTarget)
         .setColorBlendAttachment(0, blend)
         .disableDepthTest();
        return b.build(device_->device(), layout_);
    });
    if (!pipeline) return;

    VkDescriptorSet set = device_->frameSet(setLayout_);
    SceneVkDescriptorWriter writer;
    writer.writeImage(0, ssao_.visibility().view, frame.gpu.defaults.sampler);
    writer.writeImage(1, frame.gpu.targets.indirect.view, frame.gpu.defaults.sampler);
    writer.updateSet(device_->device(), set);

    const float push[4] = {frame.renderer.ssaoIntensity(), 0.0f, 0.0f, 0.0f};
    vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(frame.cmd, layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), push);
    vkCmdDraw(frame.cmd, 3, 1, 0, 0);
}

}  // namespace bro::scene::vk
