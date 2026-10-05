#include "scene/vulkan/pass_postfx.h"

#include "scene/scene_renderer.h"
#include "scene/vulkan/scene_frame.h"
#include "scene/vulkan/scene_frame_graph.h"
#include "scene/vulkan/scene_fullscreen.h"
#include "scene/vulkan/scene_targets.h"
#include "scene/vulkan/scene_vk_device.h"
#include "scene/vulkan/scene_vk_shader_compiler.h"
#include "util/log.h"

#include <algorithm>

namespace bro::scene::vk {

namespace {

constexpr VkImageUsageFlags kTargetUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

VkSampler linearClamp(VkDevice dev) {
    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = VK_FILTER_LINEAR;
    info.minFilter = VK_FILTER_LINEAR;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = info.addressModeV = info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.maxAnisotropy = 1.0f;
    VkSampler s = VK_NULL_HANDLE;
    if (vkCreateSampler(dev, &info, nullptr, &s) != VK_SUCCESS) return VK_NULL_HANDLE;
    return s;
}

/// A frame set of `views` on combined-sampler bindings 0..n-1.
VkDescriptorSet samplerSet(SceneVkDevice& device, VkDescriptorSetLayout layout, VkSampler sampler,
                           std::initializer_list<VkImageView> views) {
    VkDescriptorSet set = device.frameSet(layout);
    SceneVkDescriptorWriter writer;
    uint32_t binding = 0;
    for (VkImageView v : views) writer.writeImage(binding++, v, sampler);
    writer.updateSet(device.device(), set);
    return set;
}

/// A full-screen pass into `target`, left sampleable.
void drawInto(SceneFrame& frame, SceneVkImage& target, VkPipeline pipeline, VkPipelineLayout layout,
              VkDescriptorSet set, const void* push, uint32_t pushBytes) {
    SceneFrameGraph::transition(frame.cmd, target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    fullscreen::draw(frame.gpu.device, frame.cmd, target, pipeline, layout, set, push, pushBytes);
    SceneFrameGraph::transition(frame.cmd, target, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void destroyPipelines(SceneVkDevice& device, std::initializer_list<VkPipeline*> pipelines) {
    VkDevice dev = device.device();
    for (VkPipeline* p : pipelines) {
        if (*p) device.defer([dev, pipeline = *p] { vkDestroyPipeline(dev, pipeline, nullptr); });
        *p = VK_NULL_HANDLE;
    }
}

void destroyLayouts(VkDevice dev, std::initializer_list<VkPipelineLayout*> layouts,
                    std::initializer_list<VkDescriptorSetLayout*> setLayouts) {
    for (VkPipelineLayout* l : layouts) {
        if (*l) vkDestroyPipelineLayout(dev, *l, nullptr);
        *l = VK_NULL_HANDLE;
    }
    for (VkDescriptorSetLayout* l : setLayouts) {
        if (*l) vkDestroyDescriptorSetLayout(dev, *l, nullptr);
        *l = VK_NULL_HANDLE;
    }
}

void destroySampler(SceneVkDevice& device, VkSampler& sampler) {
    VkDevice dev = device.device();
    if (sampler) device.defer([dev, s = sampler] { vkDestroySampler(dev, s, nullptr); });
    sampler = VK_NULL_HANDLE;
}

}  // namespace

// --- Bloom + tonemap + LUT --------------------------------------------------

bool PassPostFx::setup(SceneGpu& gpu) {
    device_ = &gpu.device;
    VkDevice dev = gpu.device.device();
    sampler_ = linearClamp(dev);
    singleSetLayout_ = fullscreen::samplerSetLayout(dev, 1);
    tonemapSetLayout_ = fullscreen::samplerSetLayout(dev, 3);   // HDR, bloom, LUT
    if (!sampler_ || !singleSetLayout_ || !tonemapSetLayout_) return false;
    brightLayout_ = fullscreen::layout(dev, singleSetLayout_, sizeof(float));
    blurLayout_ = fullscreen::layout(dev, singleSetLayout_, 4 * sizeof(float));
    tonemapLayout_ = fullscreen::layout(dev, tonemapSetLayout_, sizeof(TonemapPush));

    VkShaderModule vs = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::PostFxVert);
    VkShaderModule brightFs = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::BloomBrightFrag);
    VkShaderModule blurFs = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::BlurFrag);
    VkShaderModule tonemapFs = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::TonemapFrag);
    brightPipeline_ = fullscreen::pipeline(dev, brightLayout_, vs, brightFs, SceneTargets::kHdrFormat);
    blurPipeline_ = fullscreen::pipeline(dev, blurLayout_, vs, blurFs, SceneTargets::kHdrFormat);
    tonemapPipeline_ = fullscreen::pipeline(dev, tonemapLayout_, vs, tonemapFs, SceneTargets::kLdrFormat);
    for (VkShaderModule m : {vs, brightFs, blurFs, tonemapFs}) SceneVkShaderModule::destroy(dev, m);

    // The tonemap's LUT binding samples this until a LUT is loaded.
    const uint8_t white[4] = {255, 255, 255, 255};
    if (!gpu.allocator.createTexture3D(white, 1, VK_FORMAT_R8G8B8A8_UNORM, lut_)) return false;
    return brightPipeline_ && blurPipeline_ && tonemapPipeline_;
}

void PassPostFx::resize(SceneGpu& gpu, uint32_t width, uint32_t height) {
    SceneVkAllocator& a = gpu.allocator;
    for (SceneVkImage& img : bloom_) a.destroyImage(img);
    const uint32_t hw = std::max(1u, width / 2);
    const uint32_t hh = std::max(1u, height / 2);
    for (SceneVkImage& img : bloom_) {
        if (!a.createImage(hw, hh, SceneTargets::kHdrFormat, kTargetUsage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, img)) {
            LOG_ERROR("PassPostFx: Failed creating the %ux%u bloom targets", hw, hh);
        }
    }
}

void PassPostFx::cleanup(SceneGpu& gpu) {
    for (SceneVkImage& img : bloom_) gpu.allocator.destroyImage(img);
    gpu.allocator.destroyImage(lut_);
    destroyPipelines(gpu.device, {&brightPipeline_, &blurPipeline_, &tonemapPipeline_});
    destroySampler(gpu.device, sampler_);
    destroyLayouts(gpu.device.device(), {&brightLayout_, &blurLayout_, &tonemapLayout_},
                   {&singleSetLayout_, &tonemapSetLayout_});
}

SceneVkImage& PassPostFx::source(const SceneFrame& frame) {
    return frame.dof ? frame.gpu.targets.dofHdr : frame.gpu.targets.hdr;
}

SceneVkImage& PassPostFx::output(const SceneFrame& frame) {
    return frame.tilt || frame.fxaa ? frame.gpu.targets.postLdr : frame.gpu.targets.ldr;
}

void PassPostFx::declare(const SceneFrame& frame, PassIO& io) const {
    io.sample(source(frame));
    io.colorTarget(output(frame));
}

bool PassPostFx::ensureLut(SceneGpu& gpu, const SceneRenderer& r) {
    if (r.colorLUTGeneration() != lutGeneration_) {
        lutGeneration_ = r.colorLUTGeneration();
        lutSize_ = 0;
        if (r.hasColorLUT() && r.colorLUTSize() > 1) {
            SceneVkImage lut;
            if (gpu.allocator.createTexture3D(r.colorLUTVoxels().data(), static_cast<uint32_t>(r.colorLUTSize()),
                                              VK_FORMAT_R8G8B8A8_UNORM, lut)) {
                gpu.allocator.destroyImage(lut_);
                lut_ = lut;
                lutSize_ = r.colorLUTSize();
            } else {
                LOG_ERROR("PassPostFx: Failed uploading the %d^3 colour LUT", r.colorLUTSize());
            }
        }
    }
    return lutSize_ > 1;
}

void PassPostFx::bloom(SceneFrame& frame, const SceneVkImage& hdr) {
    // Bright pass, then [0] -H-> [1] -V-> [0], reaching `strength` half-res texels.
    const float threshold = frame.renderer.bloomThreshold();
    drawInto(frame, bloom_[0], brightPipeline_, brightLayout_,
             samplerSet(*device_, singleSetLayout_, sampler_, {hdr.view}), &threshold, sizeof(threshold));
    const float strength = frame.renderer.bloomStrength();
    const float horizontal[4] = {strength / static_cast<float>(bloom_[0].width), 0.0f, 0.0f, 0.0f};
    drawInto(frame, bloom_[1], blurPipeline_, blurLayout_,
             samplerSet(*device_, singleSetLayout_, sampler_, {bloom_[0].view}), horizontal, sizeof(horizontal));
    const float vertical[4] = {0.0f, strength / static_cast<float>(bloom_[0].height), 0.0f, 0.0f};
    drawInto(frame, bloom_[0], blurPipeline_, blurLayout_,
             samplerSet(*device_, singleSetLayout_, sampler_, {bloom_[1].view}), vertical, sizeof(vertical));
}

void PassPostFx::record(SceneFrame& frame) {
    const SceneRenderer& r = frame.renderer;
    const SceneVkImage& hdr = source(frame);
    SceneVkImage& out = output(frame);
    const bool bloomOn = r.bloomEnabled() && r.bloomIntensity() > 0.0f && bloom_[1].isValid();
    if (bloomOn) bloom(frame, hdr);
    const bool lut = ensureLut(frame.gpu, r) && r.colorLUTAmount() > 0.0f;

    TonemapPush push{};
    push.exposure = r.exposure();
    push.gamma = r.gamma();
    push.bloomIntensity = bloomOn ? r.bloomIntensity() : 0.0f;
    push.tonemapMode = static_cast<int32_t>(r.toneMap());
    if (lut) {
        const auto size = static_cast<float>(lutSize_);
        push.lutAmount = r.colorLUTAmount();
        push.lutScale = (size - 1.0f) / size;
        push.lutOffset = 0.5f / size;
    }
    VkDescriptorSet set = device_->frameSet(tonemapSetLayout_);
    SceneVkDescriptorWriter writer;
    writer.writeImage(0, hdr.view, sampler_);
    writer.writeImage(1, bloomOn ? bloom_[0].view : hdr.view, sampler_);
    writer.writeImage(2, lut_.view, lut_.sampler ? lut_.sampler : sampler_);
    writer.updateSet(device_->device(), set);
    // The graph moved `out` to COLOR_ATTACHMENT_OPTIMAL (declare).
    fullscreen::draw(*device_, frame.cmd, out, tonemapPipeline_, tonemapLayout_, set, &push, sizeof(push));
    frame.ldrResult = &out;
}

// --- Tilt-shift ---------------------------------------------------------------

bool PassTiltShift::setup(SceneGpu& gpu) {
    device_ = &gpu.device;
    VkDevice dev = gpu.device.device();
    sampler_ = linearClamp(dev);
    singleSetLayout_ = fullscreen::samplerSetLayout(dev, 1);
    compositeSetLayout_ = fullscreen::samplerSetLayout(dev, 2);   // sharp, blurred
    if (!sampler_ || !singleSetLayout_ || !compositeSetLayout_) return false;
    blurLayout_ = fullscreen::layout(dev, singleSetLayout_, 4 * sizeof(float));
    compositeLayout_ = fullscreen::layout(dev, compositeSetLayout_, 5 * sizeof(float));
    VkShaderModule vs = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::PostFxVert);
    VkShaderModule blurFs = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::BlurFrag);
    VkShaderModule compositeFs =
        SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::TiltCompositeFrag);
    blurPipeline_ = fullscreen::pipeline(dev, blurLayout_, vs, blurFs, SceneTargets::kLdrFormat);
    compositePipeline_ = fullscreen::pipeline(dev, compositeLayout_, vs, compositeFs, SceneTargets::kLdrFormat);
    for (VkShaderModule m : {vs, blurFs, compositeFs}) SceneVkShaderModule::destroy(dev, m);
    return blurPipeline_ && compositePipeline_;
}

void PassTiltShift::resize(SceneGpu& gpu, uint32_t width, uint32_t height) {
    SceneVkAllocator& a = gpu.allocator;
    for (SceneVkImage& img : blur_) a.destroyImage(img);
    a.destroyImage(out_);
    const uint32_t hw = std::max(1u, width / 2);
    const uint32_t hh = std::max(1u, height / 2);
    constexpr VkMemoryPropertyFlags kLocal = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    if (!a.createImage(hw, hh, SceneTargets::kLdrFormat, kTargetUsage, kLocal, blur_[0]) ||
        !a.createImage(hw, hh, SceneTargets::kLdrFormat, kTargetUsage, kLocal, blur_[1]) ||
        !a.createImage(width, height, SceneTargets::kLdrFormat, kTargetUsage, kLocal, out_)) {
        LOG_ERROR("PassTiltShift: Failed creating the %ux%u targets", width, height);
    }
}

void PassTiltShift::cleanup(SceneGpu& gpu) {
    for (SceneVkImage& img : blur_) gpu.allocator.destroyImage(img);
    gpu.allocator.destroyImage(out_);
    destroyPipelines(gpu.device, {&blurPipeline_, &compositePipeline_});
    destroySampler(gpu.device, sampler_);
    destroyLayouts(gpu.device.device(), {&blurLayout_, &compositeLayout_}, {&singleSetLayout_, &compositeSetLayout_});
}

bool PassTiltShift::active(const SceneFrame& frame) const {
    return frame.tilt && frame.ldrResult && out_.isValid() && blur_[1].isValid();
}

void PassTiltShift::declare(const SceneFrame& frame, PassIO& io) const {
    io.sample(*frame.ldrResult);
    if (!frame.fxaa) io.colorTarget(frame.gpu.targets.ldr);
}

void PassTiltShift::record(SceneFrame& frame) {
    const SceneRenderer& r = frame.renderer;
    const SceneVkImage& sharp = *frame.ldrResult;
    const float strength = r.tiltShiftStrength();
    const float horizontal[4] = {strength / static_cast<float>(blur_[0].width), 0.0f, 0.0f, 0.0f};
    drawInto(frame, blur_[0], blurPipeline_, blurLayout_,
             samplerSet(*device_, singleSetLayout_, sampler_, {sharp.view}), horizontal, sizeof(horizontal));
    const float vertical[4] = {0.0f, strength / static_cast<float>(blur_[0].height), 0.0f, 0.0f};
    drawInto(frame, blur_[1], blurPipeline_, blurLayout_,
             samplerSet(*device_, singleSetLayout_, sampler_, {blur_[0].view}), vertical, sizeof(vertical));

    const float push[5] = {r.tiltShiftFocusCenter(), r.tiltShiftFocusWidth(), r.tiltShiftFeather(),
                           r.tiltShiftSaturation(), r.tiltShiftContrast()};
    VkDescriptorSet set = samplerSet(*device_, compositeSetLayout_, sampler_, {sharp.view, blur_[1].view});
    if (frame.fxaa) {
        drawInto(frame, out_, compositePipeline_, compositeLayout_, set, push, sizeof(push));
        frame.ldrResult = &out_;
    } else {
        fullscreen::draw(*device_, frame.cmd, frame.gpu.targets.ldr, compositePipeline_, compositeLayout_, set, push,
                         sizeof(push));
        frame.ldrResult = &frame.gpu.targets.ldr;
    }
}

// --- FXAA ---------------------------------------------------------------------

bool PassFxaa::setup(SceneGpu& gpu) {
    device_ = &gpu.device;
    VkDevice dev = gpu.device.device();
    sampler_ = linearClamp(dev);
    setLayout_ = fullscreen::samplerSetLayout(dev, 1);
    if (!sampler_ || !setLayout_) return false;
    layout_ = fullscreen::layout(dev, setLayout_, 4 * sizeof(float));
    VkShaderModule vs = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::PostFxVert);
    VkShaderModule fs = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::FxaaFrag);
    pipeline_ = fullscreen::pipeline(dev, layout_, vs, fs, SceneTargets::kLdrFormat);
    SceneVkShaderModule::destroy(dev, vs);
    SceneVkShaderModule::destroy(dev, fs);
    return pipeline_ != VK_NULL_HANDLE;
}

void PassFxaa::cleanup(SceneGpu& gpu) {
    destroyPipelines(gpu.device, {&pipeline_});
    destroySampler(gpu.device, sampler_);
    destroyLayouts(gpu.device.device(), {&layout_}, {&setLayout_});
}

bool PassFxaa::active(const SceneFrame& frame) const {
    return frame.fxaa && frame.ldrResult && frame.ldrResult != &frame.gpu.targets.ldr;
}

void PassFxaa::declare(const SceneFrame& frame, PassIO& io) const {
    io.sample(*frame.ldrResult);
    io.colorTarget(frame.gpu.targets.ldr);
}

void PassFxaa::record(SceneFrame& frame) {
    SceneVkImage& out = frame.gpu.targets.ldr;
    const float texel[4] = {1.0f / static_cast<float>(out.width), 1.0f / static_cast<float>(out.height), 0.0f, 0.0f};
    fullscreen::draw(*device_, frame.cmd, out, pipeline_, layout_,
                     samplerSet(*device_, setLayout_, sampler_, {frame.ldrResult->view}), texel, sizeof(texel));
    frame.ldrResult = &out;
}

}  // namespace bro::scene::vk
