#include "scene/vulkan/pass_color_lut.h"

#include "scene/vulkan/scene_frame.h"
#include "scene/vulkan/scene_fullscreen.h"
#include "scene/vulkan/scene_targets.h"
#include "scene/vulkan/scene_vk_device.h"
#include "scene/vulkan/scene_vk_shader_compiler.h"
#include "util/log.h"

namespace bro::scene::vk {

bool PassColorLut::setup(SceneGpu& gpu) {
    device_ = &gpu.device;
    VkDevice dev = gpu.device.device();
    setLayout_ = fullscreen::samplerSetLayout(dev, 2);   // image, LUT
    if (!setLayout_) return false;
    layout_ = fullscreen::layout(dev, setLayout_, 4 * sizeof(float));
    VkShaderModule vs = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::PostFxVert);
    VkShaderModule fs = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::ColorLutFrag);
    pipeline_ = fullscreen::pipeline(dev, layout_, vs, fs, SceneTargets::kLdrFormat);
    SceneVkShaderModule::destroy(dev, vs);
    SceneVkShaderModule::destroy(dev, fs);
    return pipeline_ != VK_NULL_HANDLE;
}

void PassColorLut::cleanup(SceneGpu& gpu) {
    VkDevice dev = gpu.device.device();
    gpu.allocator.destroyImage(lut_);
    lutSize_ = 0;
    lutGeneration_ = 0;
    gpu.device.defer([dev, p = pipeline_] {
        if (p) vkDestroyPipeline(dev, p, nullptr);
    });
    pipeline_ = VK_NULL_HANDLE;
    if (layout_) vkDestroyPipelineLayout(dev, layout_, nullptr);
    if (setLayout_) vkDestroyDescriptorSetLayout(dev, setLayout_, nullptr);
    layout_ = VK_NULL_HANDLE;
    setLayout_ = VK_NULL_HANDLE;
}

bool PassColorLut::active(const SceneFrame& frame) const {
    return frame.lut && lut_.isValid();
}

void PassColorLut::declare(const SceneFrame& frame, PassIO& io) const {
    io.sample(frame.gpu.targets.postLdr);
    io.colorTarget(frame.gpu.targets.ldr);
}

bool PassColorLut::ensureLut(SceneGpu& gpu, const SceneRenderer& r) {
    if (r.colorLUTGeneration() == lutGeneration_ && lut_.isValid()) return true;
    gpu.allocator.destroyImage(lut_);
    lutGeneration_ = r.colorLUTGeneration();
    lutSize_ = r.colorLUTSize();
    if (lutSize_ <= 1 || r.colorLUTVoxels().empty()) return false;
    if (!gpu.allocator.createTexture3D(r.colorLUTVoxels().data(), static_cast<uint32_t>(lutSize_),
                                       VK_FORMAT_R8G8B8A8_UNORM, lut_)) {
        LOG_ERROR("PassColorLut: Failed uploading the %d^3 LUT", lutSize_);
        return false;
    }
    return true;
}

void PassColorLut::record(SceneFrame& frame) {
    const SceneVkImage& input = frame.gpu.targets.postLdr;
    VkDescriptorSet set = device_->frameSet(setLayout_);
    SceneVkDescriptorWriter writer;
    writer.writeImage(0, input.view, input.sampler);
    writer.writeImage(1, lut_.view, lut_.sampler);
    writer.updateSet(device_->device(), set);
    const float size = static_cast<float>(lutSize_);
    const float push[4] = {frame.renderer.colorLUTAmount(), (size - 1.0f) / size, 0.5f / size, 0.0f};
    fullscreen::draw(*device_, frame.cmd, frame.gpu.targets.ldr, pipeline_, layout_, set, push, sizeof(push));
}

}  // namespace bro::scene::vk
