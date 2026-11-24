#include "scene/vulkan/post/scene_exposure.h"
#include "scene/vulkan/frame/scene_shader_sets.h"

#include "render/vulkan_queue.h"
#include "render/vulkan_util.h"
#include "scene/scene_renderer.h"
#include "scene/vulkan/draw/scene_vk_kernel_hook.h"
#include "scene/vulkan/frame/scene_frame.h"
#include "scene/vulkan/frame/scene_vk_descriptors.h"
#include "scene/vulkan/frame/scene_vk_device.h"
#include "scene/vulkan/frame/scene_vk_pipeline.h"
#include "scene/vulkan/lighting/scene_atmosphere.h"
#include "scene/nodes/interior_volume_node.h"
#include "scene/nodes/light_node.h"
#include <algorithm>
#include "exposure.bk.src.h"
#include "util/log.h"

#include <cmath>
#include <cstring>

namespace bro::scene::vk {

namespace {

// The bins and the count of workgroups done (post/exposure.bk).
constexpr VkDeviceSize kHistogramBytes = (exposure::kBins + 1) * sizeof(uint32_t);

VkPipelineLayout computeLayout(VkDevice dev, VkDescriptorSetLayout set, uint32_t pushBytes) {
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, pushBytes};
    VkPipelineLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    info.setLayoutCount = 1;
    info.pSetLayouts = &set;
    info.pushConstantRangeCount = 1;
    info.pPushConstantRanges = &push;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(dev, &info, nullptr, &layout) != VK_SUCCESS) return VK_NULL_HANDLE;
    return layout;
}

}  // namespace

bool SceneExposure::setup(SceneGpu& gpu) {
    device_ = &gpu.device;
    VkDevice dev = gpu.device.device();

    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxAnisotropy = 1.0f;
    if (vkCreateSampler(dev, &si, nullptr, &sampler_) != VK_SUCCESS) return false;
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    if (vkCreateSampler(dev, &si, nullptr, &tableSampler_) != VK_SUCCESS) return false;

    SceneVkDescriptorLayoutBuilder b;
    b.addBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT);
    b.addBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT);
    b.addBinding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT);
    b.addBinding(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT);
    b.addBinding(4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT);
    b.addBinding(5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT);
    setLayout_ = b.build(dev);
    if (!setLayout_) return false;
    layout_ = computeLayout(dev, setLayout_, sizeof(Push));
    if (!layout_) return false;
    VkShaderModule mod = createKernelStageModule(dev, "post/exposure.bk", kBkExposureSrc, "exposure_comp");
    pipeline_ = createComputePipeline(dev, mod, layout_, "exposure", "exposure_comp");
    SceneVkShaderModule::destroy(dev, mod);
    if (!pipeline_) return false;

    constexpr VkBufferUsageFlags kStorage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (!gpu.allocator.createBuffer(kHistogramBytes, kStorage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, histogram_) ||
        !gpu.allocator.createBuffer(kStateBytes, kStorage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, state_))
        return false;
    for (Readback& r : readbacks_) {
        if (!gpu.allocator.createBuffer(kStateBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, gpu.allocator.readbackMemory(),
                                        r.buffer) ||
            !r.buffer.mappedData)
            return false;
    }
    return true;
}

void SceneExposure::cleanup(SceneGpu& gpu) {
    VkDevice dev = gpu.device.device();
    for (Readback& r : readbacks_) gpu.allocator.destroyBuffer(r.buffer);
    gpu.allocator.destroyBuffer(histogram_);
    gpu.allocator.destroyBuffer(state_);
    if (pipeline_) gpu.device.defer([dev, pipeline = pipeline_] { destroyScenePipeline(dev, pipeline, nullptr); });
    pipeline_ = VK_NULL_HANDLE;
    if (layout_) vkDestroyPipelineLayout(dev, layout_, nullptr);
    layout_ = VK_NULL_HANDLE;
    if (setLayout_) vkDestroyDescriptorSetLayout(dev, setLayout_, nullptr);
    setLayout_ = VK_NULL_HANDLE;
    if (sampler_) gpu.device.defer([dev, s = sampler_] { vkDestroySampler(dev, s, nullptr); });
    sampler_ = VK_NULL_HANDLE;
    if (tableSampler_) gpu.device.defer([dev, s = tableSampler_] { vkDestroySampler(dev, s, nullptr); });
    tableSampler_ = VK_NULL_HANDLE;
}

void SceneExposure::readStats(SceneFrame& frame) {
    const render::VulkanQueue& queue = device_->context().queue();
    const Readback* newest = nullptr;
    for (const Readback& r : readbacks_) {
        if (r.ticket == 0 || !queue.isComplete(r.ticket)) continue;
        if (!newest || r.serial > newest->serial) newest = &r;
    }
    if (newest) {
        float s[4];
        std::memcpy(s, newest->buffer.mappedData, sizeof(s));
        ev_ = s[0];
        metered_ = s[1];
        scale_ = s[2];
    }
    frame.stats.exposureEv = ev_;
    frame.stats.meteredEv = metered_;
    frame.stats.exposureScale = scale_;
}

void SceneExposure::record(SceneFrame& frame, const SceneVkImage& hdr) {
    // A copy still pending was in a frame that never got submitted.
    for (Readback& r : readbacks_)
        if (r.pending) r = Readback{r.buffer};
    readStats(frame);
    VkCommandBuffer cmd = frame.cmd;
    VkDevice dev = device_->device();
    const ExposureSettings& e = frame.renderer.exposureSettings();
    // A debug view's values show as they are: a fixed scale of 1.
    const bool debug = frame.renderer.debugView() != 0;
    const bool metering = !debug && e.mode == ExposureSettings::Mode::Auto;

    if (!cleared_) {
        vkCmdFillBuffer(cmd, histogram_.buffer, 0, VK_WHOLE_SIZE, 0);
        vkCmdFillBuffer(cmd, state_.buffer, 0, VK_WHOLE_SIZE, 0);
        render::cmdMemoryBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        cleared_ = true;
    }
    // The last frame's tonemap read the state and its copy-out read it too;
    // its last workgroup cleared the bins: all before this frame's writes.
    render::cmdMemoryBarrier(cmd,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);

    // The incident meter reads the sun through the air at the eye: with the
    // atmosphere off it has nothing to read, and the reflected meter stands.
    const SceneAtmosphere& air = frame.gpu.atmosphere;
    const bool incident = metering && e.meter == ExposureSettings::Meter::Incident && air.active() &&
                          air.transmittanceView() != VK_NULL_HANDLE && air.skyLightView() != VK_NULL_HANDLE;
    AtmosphereGpu atm{};
    if (incident) air.fill(atm, frame.view.eye);

    VkDescriptorSet set = device_->frameSet(setLayout_);
    SceneVkDescriptorWriter writer;
    writer.writeImage(0, hdr.view, sampler_);
    writer.writeBuffer(1, histogram_.buffer, kHistogramBytes, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    writer.writeBuffer(2, state_.buffer, kStateBytes, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    if (incident)
        writer.writeImage(3, air.transmittanceView(), tableSampler_, VK_IMAGE_LAYOUT_GENERAL);
    else
        writer.writeImage(3, hdr.view, sampler_);   // (unread)
    // The volumes' readings (PassVolume), else the state (unread).
    if (incident && frame.meteredVolumes > 0)
        writer.writeBuffer(4, frame.volumeMeter, frame.meteredVolumes * SceneFrame::kVolumeMeterFloats * sizeof(float),
                           frame.volumeMeterOffset, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    else
        writer.writeBuffer(4, state_.buffer, kStateBytes, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    // The sky's light table: the atmosphere makes it with the transmittance
    // table, so it is there whenever the scene's GPU is (read only when
    // incident).
    writer.writeImage(5, air.skyLightView(), tableSampler_, VK_IMAGE_LAYOUT_GENERAL);
    writer.updateSet(dev, set);

    Push ap{};
    ap.width = static_cast<int32_t>(hdr.width);
    ap.height = static_cast<int32_t>(hdr.height);
    ap.minLog2 = exposure::kMinLog2;
    ap.maxLog2 = exposure::kMaxLog2;
    ap.mode = metering ? 1.0f : 0.0f;
    ap.fixedEv = e.ev100;
    ap.fixedScale = debug ? 1.0f : exposure::scaleForEv(static_cast<double>(e.ev100) - e.compensation);
    ap.compensation = e.compensation;
    ap.dt = frame.renderer.exposureDt();
    ap.speedUp = e.speedUp;
    ap.speedDown = e.speedDown;
    ap.minEv = e.minEv;
    ap.maxEv = e.maxEv;
    ap.lowPercent = e.lowPercent;
    ap.highPercent = e.highPercent;
    if (incident) {
        const double ox = atm.origin[0], oy = atm.origin[1], oz = atm.origin[2];
        const double r = std::sqrt(ox * ox + oy * oy + oz * oz);
        ap.incident = 1.0f;
        ap.rb = atm.shell[0];
        ap.rt = atm.shell[1];
        ap.r = static_cast<float>(r);
        ap.muS = r > 0.0 ? static_cast<float>((ox * atm.sunDir[0] + oy * atm.sunDir[1] + oz * atm.sunDir[2]) / r) : 1.0f;
        ap.sunLux = 0.2126f * atm.sunIlluminance[0] + 0.7152f * atm.sunIlluminance[1] + 0.0722f * atm.sunIlluminance[2];
        ap.sunRadius = atm.sunIlluminance[3];
        ap.incidentEvOffset = static_cast<float>(exposure::incidentEvOffset());
        ap.brighter = exposure::kIncidentBrighter;
        ap.darker = exposure::kIncidentDarker;
        ap.volumes = static_cast<int32_t>(frame.meteredVolumes);
        const InteriorEnclosureSample enc = evaluateInteriorEnclosures(
            frame.renderer.frameInteriorVolumes(), frame.view.eye, true);
        ap.enclosureSunVis = enc.sunVisibility;
        ap.enclosureSkyVis = enc.skyVisibility;
        // A level meter faces up: the fill as a floor takes it (lighting.bk's
        // env_fill, 1.35 of it from above), and inside, the room's own lamps
        // on that level surface as they light it (a lamp's luminous
        // intensity over the square of its distance, cosine and window as
        // the lit shaders take them), weighted by how far in the eye is.
        float lux = 1.35f * (0.2126f * enc.ambient.x + 0.7152f * enc.ambient.y + 0.0722f * enc.ambient.z);
        if (enc.weight > 0.0f) {
            float lamps = 0.0f;
            const bromath::Vec3& eye = frame.view.eye;
            for (const LightNode* l : frame.renderer.frameLights()) {
                if (!l || l->kind() == LightNode::Kind::Directional) continue;
                const bromath::Mat4& m = l->worldMatrix();
                const bromath::Vec3 to{m.at(0, 3) - eye.x, m.at(1, 3) - eye.y, m.at(2, 3) - eye.z};
                const float d = std::sqrt(bromath::vdot(to, to));
                const float range = l->range();
                if (d < 1e-4f || (range > 0.0f && d > range) || to.y <= 0.0f) continue;
                float att = 1.0f / (d * d + 1.0f);
                if (range > 0.0f) {
                    const float t = d / range, win = std::clamp(1.0f - t * t * t * t, 0.0f, 1.0f);
                    att *= win * win;
                }
                if (l->kind() == LightNode::Kind::Spot) {
                    bromath::Vec3 dir = l->direction();
                    const float len = std::sqrt(bromath::vdot(dir, dir));
                    if (len > 1e-6f) dir = dir * (1.0f / len);
                    const float c = -bromath::vdot(to, dir) / d, ci = std::cos(l->innerAngle()), co = std::cos(l->outerAngle());
                    const float s = std::clamp((c - co) / std::max(ci - co, 1e-4f), 0.0f, 1.0f);
                    att *= s * s * (3.0f - 2.0f * s);
                }
                const bromath::Vec3& c = l->color();
                lamps += (0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z) * l->intensity() * att * (to.y / d);
            }
            // (π: env_fill is a luminance a unit albedo; lux / π is the same scale.)
            lux += enc.weight * lamps / 3.14159265f;
        }
        ap.enclosureAmbientLux = lux;
    } else {
        ap.enclosureSunVis = 1.0f;
        ap.enclosureSkyVis = 1.0f;
        ap.enclosureAmbientLux = 0.0f;
    }
    bindScenePipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ap), &ap);
    // Auto: an invocation a pixel of every 4 x 4 block, 16 x 16 a workgroup.
    // Fixed: the one workgroup that writes the state.
    if (metering)
        vkCmdDispatch(cmd, (hdr.width + 63) / 64, (hdr.height + 63) / 64, 1);
    else
        vkCmdDispatch(cmd, 1, 1, 1);
    render::cmdMemoryBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT);

    // A copy for the CPU, into a slot no frame in flight still writes.
    const render::VulkanQueue& queue = device_->context().queue();
    for (Readback& r : readbacks_) {
        if (r.pending || (r.ticket != 0 && !queue.isComplete(r.ticket))) continue;
        // Keep the newest finished copy until a newer one lands.
        bool newestDone = r.ticket != 0;
        for (const Readback& o : readbacks_)
            if (&o != &r && o.ticket != 0 && queue.isComplete(o.ticket) && o.serial > r.serial) newestDone = false;
        if (newestDone) continue;
        VkBufferCopy region{0, 0, kStateBytes};
        vkCmdCopyBuffer(cmd, state_.buffer, r.buffer.buffer, 1, &region);
        render::cmdBufferBarrier(cmd, r.buffer.buffer, 0, VK_WHOLE_SIZE, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
        r.pending = true;
        r.ticket = 0;
        r.serial = ++serial_;
        break;
    }
}

void SceneExposure::submitted(uint64_t ticket) {
    for (Readback& r : readbacks_) {
        if (!r.pending) continue;
        r.pending = false;
        r.ticket = ticket;
    }
}

}  // namespace bro::scene::vk
