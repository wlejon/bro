#include "scene/vulkan/draw/pass_volume.h"
#include "scene/vulkan/frame/scene_shader_sets.h"

#include "render/vulkan_context.h"
#include "render/vulkan_util.h"
#include "scene/vulkan/frame/scene_vk_allocator.h"
#include "scene/vulkan/frame/scene_vk_descriptors.h"
#include "scene/vulkan/draw/scene_mesh_drawer.h"
#include "scene/vulkan/draw/scene_vk_custom_shader.h"
#include "scene/vulkan/draw/scene_vk_kernel_hook.h"
#include "scene/vulkan/frame/scene_defaults.h"
#include "scene/vulkan/frame/scene_frame.h"
#include "scene/vulkan/frame/scene_frame_graph.h"
#include "scene/vulkan/frame/scene_fullscreen.h"
#include "scene/vulkan/frame/scene_targets.h"
#include "scene/vulkan/frame/scene_vk_device.h"
#include "scene/vulkan/frame/scene_vk_pipeline.h"
#include "scene/vulkan/frame/scene_vk_shader_compiler.h"
#include "postfx.bk.src.h"
#include "util/log.h"

#include <algorithm>
#include <cstring>

namespace bro::scene::vk {

namespace {
// The volumes the exposure meter reads, the first in draw order.
constexpr uint32_t kMeteredVolumes = 8;
// The medium's depth beside its light.
constexpr VkFormat kDepthFormat = VK_FORMAT_R32_SFLOAT;

TargetFormat volumeTarget() {
    TargetFormat t = TargetFormat::colorOnly(SceneTargets::kHdrFormat);
    t.color[1] = kDepthFormat;
    t.colorCount = 2;
    return t;
}
}  // namespace

bool PassVolume::setup(SceneGpu& gpu) {
    device_ = &gpu.device;
    VkDevice dev = gpu.device.device();
    VkSamplerCreateInfo sampler{};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = VK_FILTER_LINEAR;
    sampler.minFilter = VK_FILTER_LINEAR;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(dev, &sampler, nullptr, &sampler_) != VK_SUCCESS) return false;

    // This render's light, its depth, the history; the uniforms.
    SceneVkDescriptorLayoutBuilder builder;
    for (uint32_t i = 0; i < 3; ++i)
        builder.addBinding(i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
    builder.addBinding(3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
    setLayout_ = builder.build(dev);
    if (!setLayout_) return false;
    layout_ = fullscreen::layout(dev, setLayout_, 0);
    VkShaderModule vs = createKernelStageModule(dev, "post/postfx.bk", kBkPostFxSrc, "postfx_vert");
    VkShaderModule fs = createKernelStageModule(dev, "post/postfx.bk", kBkPostFxSrc, "volume_resolve_frag");
    resolve_ = fullscreen::pipeline(dev, layout_, vs, fs, SceneTargets::kHdrFormat, "volume resolve", "postfx_vert",
                                    "volume_resolve_frag");
    for (VkShaderModule m : {vs, fs}) SceneVkShaderModule::destroy(dev, m);
    return resolve_ != VK_NULL_HANDLE;
}

void PassVolume::resize(SceneGpu& gpu, uint32_t width, uint32_t height) {
    for (SceneVkImage* img : {&medium_, &mediumDepth_, &history_[0], &history_[1]}) gpu.allocator.destroyImage(*img);
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    bool ok = gpu.allocator.createImage(width, height, SceneTargets::kHdrFormat, usage,
                                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, medium_) &&
              gpu.allocator.createImage(width, height, kDepthFormat, usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                        mediumDepth_);
    for (SceneVkImage& img : history_)
        ok = ok && gpu.allocator.createImage(width, height, SceneTargets::kHdrFormat, usage,
                                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, img);
    if (!ok) LOG_ERROR("PassVolume: Failed creating the %ux%u volume targets", width, height);
    haveHistory_ = false;
}

void PassVolume::cleanup(SceneGpu& gpu) {
    VkDevice dev = gpu.device.device();
    for (SceneVkImage* img : {&medium_, &mediumDepth_, &history_[0], &history_[1]}) gpu.allocator.destroyImage(*img);
    gpu.device.defer([dev, s = sampler_, p = resolve_] {
        if (s) vkDestroySampler(dev, s, nullptr);
        if (p) destroyScenePipeline(dev, p, nullptr);
    });
    sampler_ = VK_NULL_HANDLE;
    resolve_ = VK_NULL_HANDLE;
    if (layout_) vkDestroyPipelineLayout(dev, layout_, nullptr);
    if (setLayout_) vkDestroyDescriptorSetLayout(dev, setLayout_, nullptr);
    layout_ = VK_NULL_HANDLE;
    setLayout_ = VK_NULL_HANDLE;
}

bool PassVolume::active(const SceneFrame& frame) const {
    return !frame.lists.volumes.empty() && medium_.isValid() && mediumDepth_.isValid() && history_[0].isValid() &&
           history_[1].isValid();
}

void PassVolume::declare(const SceneFrame& frame, PassIO& io) const {
    io.sample(frame.gpu.targets.depthSnapshot);
}

bool PassVolumeNear::setup(SceneGpu& gpu) {
    device_ = &gpu.device;
    allocator_ = &gpu.allocator;
    return true;
}

void PassVolumeNear::resize(SceneGpu&, uint32_t width, uint32_t height) {
    width_ = width;
    height_ = height;
}

void PassVolumeNear::cleanup(SceneGpu& gpu) {
    for (auto& [id, grid] : grids_) {
        gpu.allocator.destroyImage(grid.light);
        gpu.allocator.destroyImage(grid.depth);
    }
    grids_.clear();
}

bool PassVolumeNear::active(const SceneFrame& frame) const {
    return !frame.lists.volumes.empty() || !grids_.empty();
}

void PassVolumeNear::declare(const SceneFrame&, PassIO&) const {}

const PassVolumeNear::Grid* PassVolumeNear::grid(const SceneFrame& frame, uint32_t nodeId) const {
    auto it = grids_.find(nodeId);
    return it != grids_.end() && it->second.frame == frame.view.frameIndex && it->second.light.isValid()
               ? &it->second
               : nullptr;
}

PassVolumeNear::Grid* PassVolumeNear::make(SceneFrame& frame, uint32_t nodeId) {
    const uint32_t gx = (width_ + kNearTile - 1) / kNearTile;
    const uint32_t gy = (height_ + kNearTile - 1) / kNearTile;
    if (gx == 0 || gy == 0) return nullptr;
    Grid& grid = grids_[nodeId];
    grid.frame = frame.view.frameIndex;
    if (grid.light.isValid() && grid.light.width == gx && grid.light.height == gy) return &grid;
    allocator_->destroyImage(grid.light);
    allocator_->destroyImage(grid.depth);
    if (!allocator_->createStorageImage(gx, gy, kNearSlices, VK_FORMAT_R16G16B16A16_SFLOAT, grid.light, false) ||
        !allocator_->createStorageImage(gx, gy, kNearSlices, VK_FORMAT_R32_SFLOAT, grid.depth, false)) {
        LOG_ERROR("PassVolumeNear: Failed creating a %ux%ux%u grid", gx, gy, kNearSlices);
        allocator_->destroyImage(grid.light);
        allocator_->destroyImage(grid.depth);
        return nullptr;
    }
    return &grid;
}

void PassVolumeNear::record(SceneFrame& frame) {
    VkCommandBuffer cmd = frame.cmd;
    bool any = false;
    uint32_t index = 0;
    for (const MeshDraw& draw : frame.lists.volumes) {
        const uint32_t at = index++;
        if (!frame.gpu.meshes.volumeHasMedium(draw)) continue;
        Grid* grid = make(frame, draw.nodeId);
        if (!grid) continue;
        if (!any) {
            // Last render's pixels done reading the grids before they are
            // written again.
            render::cmdMemoryBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
            any = true;
        }
        VkDescriptorSet set = device_->frameSet(frame.gpu.meshes.volumeNearSetLayout());
        SceneVkDescriptorWriter writer;
        writer.writeImage(6, grid->light.view, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL,
                          VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        writer.writeImage(7, grid->depth.view, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL,
                          VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        writer.updateSet(device_->device(), set);
        // A pass with no attachments over the grid's cells: one fragment a
        // cell, writing its column.
        const uint32_t gx = grid->light.width, gy = grid->light.height;
        VkRenderingInfo info{};
        info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        info.renderArea = {{0, 0}, {gx, gy}};
        info.layerCount = 1;
        device_->cmdBeginRendering(cmd, &info);
        const VkViewport vp{0.0f, 0.0f, static_cast<float>(gx), static_cast<float>(gy), 0.0f, 1.0f};
        const VkRect2D scissor{{0, 0}, {gx, gy}};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        frame.gpu.meshes.recordVolumeNear(cmd, frame.cameraSet, frame.lightingSet, set, draw, at);
        device_->cmdEndRendering(cmd);
    }
    // Grids of volumes no longer drawn go.
    for (auto it = grids_.begin(); it != grids_.end();) {
        if (it->second.frame == frame.view.frameIndex) {
            ++it;
            continue;
        }
        allocator_->destroyImage(it->second.light);
        allocator_->destroyImage(it->second.depth);
        it = grids_.erase(it);
    }
    if (any)
        render::cmdMemoryBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
}

void PassVolume::record(SceneFrame& frame) {
    // The meter's slots in this frame's upload memory, each "no medium" (no
    // light, all the sun) until its volume writes it: a volume whose program
    // failed leaves the meter as it was. Without the memory nothing draws
    // (frameUpload has said so).
    const uint32_t metered = std::min<uint32_t>(static_cast<uint32_t>(frame.lists.volumes.size()), kMeteredVolumes);
    constexpr uint32_t kFloats = SceneFrame::kVolumeMeterFloats;
    const VkDeviceSize bytes = metered * kFloats * sizeof(float);
    const VkDeviceSize align =
        frame.gpu.device.context().deviceProperties().limits.minStorageBufferOffsetAlignment;
    render::UploadSlice meter = frame.gpu.device.frameUpload(bytes, std::max<VkDeviceSize>(align, 16));
    if (!meter) return;
    const float none[8] = {0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f};
    for (uint32_t i = 0; i < metered * SceneFrame::kVolumeMeterRays; ++i)
        std::memcpy(static_cast<float*>(meter.mapped) + 8 * i, none, sizeof(none));
    frame.volumeMeter = meter.buffer;
    frame.volumeMeterOffset = meter.offset;
    frame.meteredVolumes = metered;

    // The opaque depth in the material set's first slot, the rest its
    // defaults, and the meter's slots.
    const SceneDefaults& d = frame.gpu.defaults;
    const SceneVkImage& depth = frame.gpu.targets.depthSnapshot;
    VkDescriptorSet set = frame.gpu.device.frameSet(d.materialLayout);
    SceneVkDescriptorWriter writer;
    writer.writeImage(0, depth.view, depth.sampler);
    writer.writeImage(1, d.flatNormal.view, d.sampler);
    writer.writeImage(2, d.white.view, d.sampler);
    writer.writeImage(3, d.black.view, d.sampler);
    writer.writeImage(4, d.white.view, d.sampler);
    writer.writeBuffer(5, meter.buffer, bytes, meter.offset, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    writer.updateSet(frame.gpu.device.device(), set);

    // The volumes into their own target: no light, all passed, no depth.
    VkCommandBuffer cmd = frame.cmd;
    SceneFrameGraph::transition(cmd, medium_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    SceneFrameGraph::transition(cmd, mediumDepth_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo att[2]{};
    for (int i = 0; i < 2; ++i) {
        att[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        att[i].imageView = i == 0 ? medium_.view : mediumDepth_.view;
        att[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    att[0].clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    att[1].clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
    const uint32_t width = medium_.width, height = medium_.height;
    VkRenderingInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    info.renderArea = {{0, 0}, {width, height}};
    info.layerCount = 1;
    info.colorAttachmentCount = 2;
    info.pColorAttachments = att;
    frame.gpu.device.cmdBeginRendering(cmd, &info);
    const VkViewport vp{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, {width, height}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    const TargetFormat target = volumeTarget();
    uint32_t index = 0;
    for (const MeshDraw& draw : frame.lists.volumes) {
        // A volume with a near grid reads it as images 1 and 2.
        VkDescriptorSet drawSet = set;
        if (const PassVolumeNear::Grid* grid = near_.grid(frame, draw.nodeId)) {
            drawSet = frame.gpu.device.frameSet(d.materialLayout);
            SceneVkDescriptorWriter gw;
            gw.writeImage(0, depth.view, depth.sampler);
            gw.writeImage(1, grid->light.view, sampler_, VK_IMAGE_LAYOUT_GENERAL);
            gw.writeImage(2, grid->depth.view, sampler_, VK_IMAGE_LAYOUT_GENERAL);
            gw.writeImage(3, d.black.view, d.sampler);
            gw.writeImage(4, d.white.view, d.sampler);
            gw.writeBuffer(5, meter.buffer, bytes, meter.offset, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
            gw.updateSet(frame.gpu.device.device(), drawSet);
        }
        frame.gpu.meshes.recordVolume(cmd, target, frame.cameraSet, frame.lightingSet, drawSet, draw, index);
        ++index;
    }
    frame.gpu.device.cmdEndRendering(cmd);
    frame.drewContent = true;
    SceneFrameGraph::transition(cmd, medium_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    SceneFrameGraph::transition(cmd, mediumDepth_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // Reconstructed with last render's into the other history image, when
    // that was the render before this one and of the same volumes.
    uint64_t ids = 1469598103934665603ull;
    for (const MeshDraw& draw : frame.lists.volumes) ids = (ids ^ draw.nodeId) * 1099511628211ull;
    const bool follows = haveHistory_ && frame.view.frameIndex == lastIndex_ + 1 && ids == lastVolumes_;
    lastVolumes_ = ids;
    const uint32_t prev = current_;
    current_ ^= 1u;
    Uniforms u{};
    std::memcpy(u.prevViewProj, prevViewProj_.data, sizeof(u.prevViewProj));
    const bromath::Mat4 inv = bromath::minverse(frame.view.relViewProj);
    std::memcpy(u.invViewProj, inv.data, sizeof(u.invViewProj));
    const bromath::Vec3 delta = frame.view.eye - prevEye_;
    u.eyeDelta[0] = delta.x;
    u.eyeDelta[1] = delta.y;
    u.eyeDelta[2] = delta.z;
    u.eyeDelta[3] = follows ? kBlend : 1.0f;
    const VkDescriptorBufferInfo ubo = device_->frameUniform(&u, sizeof(u));
    SceneFrameGraph::transition(cmd, history_[prev], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    VkDescriptorSet resolveSet = device_->frameSet(setLayout_);
    SceneVkDescriptorWriter rw;
    rw.writeImage(0, medium_.view, sampler_);
    rw.writeImage(1, mediumDepth_.view, sampler_);
    rw.writeImage(2, history_[prev].view, sampler_);
    rw.writeBuffer(3, ubo.buffer, ubo.range, ubo.offset);
    rw.updateSet(device_->device(), resolveSet);
    SceneVkImage& out = history_[current_];
    SceneFrameGraph::transition(cmd, out, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    fullscreen::draw(*device_, cmd, out, resolve_, layout_, resolveSet);
    SceneFrameGraph::transition(cmd, out, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    haveHistory_ = true;
    lastIndex_ = frame.view.frameIndex;
    prevViewProj_ = frame.view.relViewProj;
    prevEye_ = frame.view.eye;
}

bool PassVolumeComposite::setup(SceneGpu& gpu) {
    device_ = &gpu.device;
    VkDevice dev = gpu.device.device();
    setLayout_ = fullscreen::samplerSetLayout(dev, 1);
    if (!setLayout_) return false;
    layout_ = fullscreen::layout(dev, setLayout_, 0);
    vs_ = createKernelStageModule(dev, "post/postfx.bk", kBkPostFxSrc, "postfx_vert");
    fs_ = createKernelStageModule(dev, "post/postfx.bk", kBkPostFxSrc, "volume_composite_frag");
    return layout_ && vs_ && fs_;
}

void PassVolumeComposite::cleanup(SceneGpu& gpu) {
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

bool PassVolumeComposite::active(const SceneFrame& frame) const {
    return volumes_.active(frame);
}

void PassVolumeComposite::declare(const SceneFrame&, PassIO& io) const {
    io.hdr();
}

void PassVolumeComposite::record(SceneFrame& frame) {
    VkPipeline pipeline = pipelines_.get(0, frame.hdrTarget, [&] {
        // Over the scene: dst * transmittance (the source alpha) + the
        // volumes' light; the scene's alpha is kept.
        VkPipelineColorBlendAttachmentState blend{};
        blend.blendEnable = VK_TRUE;
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blend.colorBlendOp = VK_BLEND_OP_ADD;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.alphaBlendOp = VK_BLEND_OP_ADD;
        blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                               VK_COLOR_COMPONENT_A_BIT;
        SceneVkPipelineBuilder b;
        b.setLabel("volume box")
         .setShaderStages(vs_, fs_, "postfx_vert", "volume_composite_frag")
         .setInputTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
         .setPolygonMode(VK_POLYGON_MODE_FILL)
         .setCullMode(VK_CULL_MODE_NONE)
         .setTarget(frame.hdrTarget)
         .disableBlending(frame.hdrTarget.colorCount)
         .setColorBlendAttachment(0, blend)
         .disableDepthTest();
        return b.build(device_->device(), layout_);
    });
    if (!pipeline) return;
    VkDescriptorSet set = device_->frameSet(setLayout_);
    SceneVkDescriptorWriter writer;
    writer.writeImage(0, volumes_.result().view, volumes_.sampler());
    writer.updateSet(device_->device(), set);
    bindScenePipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set, 0, nullptr);
    vkCmdDraw(frame.cmd, 3, 1, 0, 0);
}

SceneMeshDrawer::CustomProgram* SceneMeshDrawer::volumeProgram(const CustomShaderState& cs) {
    const std::string key = cs.key + "\x1evolume";
    auto it = custom_.find(key);
    if (it != custom_.end()) return it->second->failed ? nullptr : it->second.get();

    auto prog = std::make_unique<CustomProgram>();
    std::string err;
    if (cs.isModule()) {
        KernelVolumeHook hook;
        bool ok = compileKernelVolumeHook(cs.module, cs.moduleFile, cs.modules, hook, err);
        if (ok) {
            prog->iface = std::move(hook.iface);
            prog->medium = hook.medium;
            prog->fs = SceneVkShaderModule::create(device_, hook.main.spirv);
            prog->fsEntry = hook.main.entry;
            if (hook.medium) {
                prog->fsNear = SceneVkShaderModule::create(device_, hook.nearPass.spirv);
                prog->fsNearEntry = hook.nearPass.entry;
            }
            ok = prog->fs != VK_NULL_HANDLE && (!hook.medium || prog->fsNear != VK_NULL_HANDLE);
            if (!ok) err = "Failed to create VkShaderModule";
        }
        if (!ok) {
            LOG_ERROR("SceneMeshDrawer: Failed compiling a volume module: %s", err.c_str());
            prog->failed = true;
            custom_.emplace(key, std::move(prog));
            return nullptr;
        }
        return custom_.emplace(key, std::move(prog)).first->second.get();
    }
    prog->medium = SceneVkCustomShader::volumeHasMedium(cs.fragmentChunk);
    if (!prog->iface.parse(cs.vertexChunk, cs.fragmentChunk, err) ||
        !SceneVkCustomShader::compileVolumeModule(device_, prog->iface, false, prog->fs, err) ||
        (prog->medium && !SceneVkCustomShader::compileVolumeModule(device_, prog->iface, true, prog->fsNear, err))) {
        // Logged once; the volume draws nothing from now on.
        LOG_ERROR("SceneMeshDrawer: Failed compiling a volume shader: %s", err.c_str());
        prog->failed = true;
        custom_.emplace(key, std::move(prog));
        return nullptr;
    }
    return custom_.emplace(key, std::move(prog)).first->second.get();
}

void SceneMeshDrawer::recordVolume(VkCommandBuffer cmd, const TargetFormat& target, VkDescriptorSet cameraSet,
                                   VkDescriptorSet lightingSet, VkDescriptorSet depthSet, const MeshDraw& draw,
                                   uint32_t index) {
    if (!draw.volume || !draw.custom || !draw.customSet) return;
    CustomProgram* prog = volumeProgram(*draw.custom);
    if (!prog) return;
    const bool atmosphere = atmosphere_;
    VkPipeline pipeline = prog->pipelines.get(atmosphere ? kAtmosphereVariant : 0u, target, [&] {
        // Over the volumes before it: their light * its transmittance (the
        // source alpha) + its light, the transmittances multiplied; the
        // medium's depth overwritten.
        VkPipelineColorBlendAttachmentState blend{};
        blend.blendEnable = VK_TRUE;
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blend.colorBlendOp = VK_BLEND_OP_ADD;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blend.alphaBlendOp = VK_BLEND_OP_ADD;
        blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                               VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendAttachmentState overwrite{};
        overwrite.colorWriteMask = VK_COLOR_COMPONENT_R_BIT;
        SceneVkPipelineBuilder b;
        b.addShaderStage(VK_SHADER_STAGE_VERTEX_BIT, vsVolume_)
         .addShaderStage(VK_SHADER_STAGE_FRAGMENT_BIT, prog->fs, prog->fsEntry.c_str())
         .setVertexInput({}, {})
         .setInputTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
         .setPolygonMode(VK_POLYGON_MODE_FILL)
         .setCullMode(VK_CULL_MODE_NONE)
         .setTarget(target)
         .setColorBlendAttachment(0, blend)
         .setColorBlendAttachment(1, overwrite)
         .setSpecialization(kAtmosphereConstantId, atmosphere)
         .setLabel("volume")
         .disableDepthTest();
        return b.build(device_, layout_);
    });
    if (!pipeline) return;
    bindScenePipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    const VkDescriptorSet sets[3] = {cameraSet, lightingSet, depthSet};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 3, sets, 0, nullptr);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 4, 1, &draw.customSet, 0, nullptr);
    // The volume's place as its first instance: volume.frag's meter slot.
    vkCmdDraw(cmd, 3, 1, 0, index);
}

bool SceneMeshDrawer::volumeHasMedium(const MeshDraw& draw) {
    if (!draw.volume || !draw.custom || !draw.customSet) return false;
    const CustomProgram* prog = volumeProgram(*draw.custom);
    return prog && prog->medium;
}

void SceneMeshDrawer::recordVolumeNear(VkCommandBuffer cmd, VkDescriptorSet cameraSet, VkDescriptorSet lightingSet,
                                       VkDescriptorSet nearSet, const MeshDraw& draw, uint32_t index) {
    if (!volumeHasMedium(draw)) return;
    CustomProgram* prog = volumeProgram(*draw.custom);
    const bool atmosphere = atmosphere_;
    VkPipeline pipeline = prog->nearPipelines.get(atmosphere ? kAtmosphereVariant : 0u, TargetFormat{}, [&] {
        SceneVkPipelineBuilder b;
        b.addShaderStage(VK_SHADER_STAGE_VERTEX_BIT, vsVolume_)
         .addShaderStage(VK_SHADER_STAGE_FRAGMENT_BIT, prog->fsNear, prog->fsNearEntry.c_str())
         .setVertexInput({}, {})
         .setInputTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
         .setPolygonMode(VK_POLYGON_MODE_FILL)
         .setCullMode(VK_CULL_MODE_NONE)
         .setTarget(TargetFormat{})
         .setSpecialization(kAtmosphereConstantId, atmosphere)
         .setLabel("volume near")
         .disableDepthTest();
        return b.build(device_, nearLayout_);
    });
    if (!pipeline) return;
    bindScenePipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    const VkDescriptorSet sets[3] = {cameraSet, lightingSet, nearSet};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, nearLayout_, 0, 3, sets, 0, nullptr);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, nearLayout_, 4, 1, &draw.customSet, 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, index);
}

}  // namespace bro::scene::vk
