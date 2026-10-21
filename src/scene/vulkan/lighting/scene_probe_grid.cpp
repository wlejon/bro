#include "scene/vulkan/lighting/scene_probe_grid.h"

#include "render/vulkan_queue.h"
#include "render/vulkan_util.h"
#include "scene/nodes/mesh_node.h"
#include "scene/nodes/plants_node.h"
#include "scene/scene_renderer.h"
#include "scene/vulkan/draw/scene_mesh_drawer.h"
#include "scene/vulkan/frame/scene_frame.h"
#include "scene/vulkan/frame/scene_frame_graph.h"
#include "scene/vulkan/frame/scene_gpu_resources.h"
#include "scene/vulkan/frame/scene_targets.h"
#include "scene/vulkan/frame/scene_vk_descriptors.h"
#include "scene/vulkan/frame/scene_vk_device.h"
#include "scene/vulkan/frame/scene_vk_pipeline.h"
#include "scene/vulkan/frame/scene_vk_shader_compiler.h"
#include "scene/vulkan/shadow/scene_caster_drawer.h"
#include "util/log.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bro::scene::vk {

namespace {

constexpr BuiltinSceneShader kShaders[] = {BuiltinSceneShader::ProbeHeightComp, BuiltinSceneShader::ProbeSplatComp,
                                           BuiltinSceneShader::ProbeResolveComp, BuiltinSceneShader::ProbeSunComp};
constexpr const char* kNames[] = {"probe height", "probe splat", "probe resolve", "probe sun"};

// The sun turns the canopy's light when it moves by more than this (a
// quarter of a degree, as cosine): a voxel's shade does not move a texel.
constexpr float kSunTurnCos = 0.99999048f;

constexpr uint32_t kVoxelsPerCascade = PG_VOXELS * PG_VOXELS * PG_LAYERS_Y;
constexpr VkDeviceSize kSumBytesPerCascade = VkDeviceSize{kVoxelsPerCascade} * 16u;

float halfToFloat(uint16_t h) {
    const uint32_t sign = (h >> 15) & 1u, exp = (h >> 10) & 31u, man = h & 1023u;
    float v;
    if (exp == 0) v = std::ldexp(static_cast<float>(man), -24);
    else if (exp == 31) v = man ? NAN : INFINITY;
    else v = std::ldexp(static_cast<float>(man | 1024u), static_cast<int>(exp) - 25);
    return sign ? -v : v;
}

/// What the ground's capture draws: what is opaque and stands still.
bool groundCaster(const MeshDraw& d) {
    const uint32_t flags = static_cast<uint32_t>(d.push.pbrParams[3]);
    return (d.kind == MeshKind::Static || d.kind == MeshKind::Instanced) && !d.translucent && !d.shadowOnly &&
           !d.lines && !d.noDepthTest && d.indirect == VK_NULL_HANDLE && d.shadowMotion == ShadowMotion::Still &&
           (flags & mesh_flags::kUnlit) == 0u;
}

}  // namespace

bool SceneProbeGrid::setup(SceneGpu& gpu) {
    device_ = &gpu.device;
    VkDevice dev = gpu.device.device();
    grid_.configure(ProbeGridConfig{});

    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxAnisotropy = 1.0f;
    if (vkCreateSampler(dev, &si, nullptr, &linear_) != VK_SUCCESS) return false;
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    if (vkCreateSampler(dev, &si, nullptr, &nearest_) != VK_SUCCESS) return false;

    SceneVkDescriptorLayoutBuilder b;
    b.addBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT);
    b.addBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT);
    b.addBinding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT);
    b.addBinding(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT);
    b.addBinding(4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT);
    b.addBinding(5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT);
    b.addBinding(6, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT);
    b.addBinding(7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT);
    setLayout_ = b.build(dev);
    if (!setLayout_) return false;
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    VkPipelineLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    li.setLayoutCount = 1;
    li.pSetLayouts = &setLayout_;
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(dev, &li, nullptr, &layout_) != VK_SUCCESS) return false;
    for (uint32_t s = 0; s < kStages; ++s) {
        pipelines_[s] = createComputePipeline(dev, kShaders[s], layout_, kNames[s]);
        if (!pipelines_[s]) return false;
    }

    SceneVkAllocator& a = gpu.allocator;
    if (!a.createImage(PG_HEIGHT, PG_HEIGHT * PG_MAX_CASCADES, SceneTargets::kDepthFormat,
                       VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, captureDepth_, 1, VK_SAMPLE_COUNT_1_BIT,
                       VK_IMAGE_ASPECT_DEPTH_BIT) ||
        !a.createStorageImage(PG_HEIGHT, PG_HEIGHT * PG_MAX_CASCADES, 1, VK_FORMAT_R32_SFLOAT, height_, false) ||
        !a.createStorageImage(PG_VOXELS, PG_VOXELS, PG_LAYERS_Y * PG_MAX_CASCADES, VK_FORMAT_R16G16B16A16_SFLOAT,
                              medium_, false) ||
        !a.createStorageImage(PG_VOXELS, PG_VOXELS, PG_LAYERS_Y * PG_MAX_CASCADES, VK_FORMAT_R16_SFLOAT, sun_,
                              false) ||
        !a.createBuffer(kSumBytesPerCascade * PG_MAX_CASCADES,
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, sums_)) {
        LOG_ERROR("SceneProbeGrid: failed creating the probe grid's images");
        return false;
    }
    // A cascade's world is its volumes: the CPU half's error is what it
    // costs, so the guess is a whole cascade's.
    kind_ = gpu.world.addKind(this, {"probe grid world", WorldSchedule::Raster, 0.2f});
    return kind_ >= 0;
}

void SceneProbeGrid::cleanup(SceneGpu& gpu) {
    VkDevice dev = gpu.device.device();
    SceneVkAllocator& a = gpu.allocator;
    for (SceneVkImage* img : {&captureDepth_, &height_, &medium_, &sun_}) a.destroyImage(*img);
    a.destroyBuffer(sums_);
    for (VkPipeline& p : pipelines_) {
        if (p) gpu.device.defer([dev, pipeline = p] { vkDestroyPipeline(dev, pipeline, nullptr); });
        p = VK_NULL_HANDLE;
    }
    if (layout_) vkDestroyPipelineLayout(dev, layout_, nullptr);
    if (setLayout_) vkDestroyDescriptorSetLayout(dev, setLayout_, nullptr);
    layout_ = VK_NULL_HANDLE;
    setLayout_ = VK_NULL_HANDLE;
    for (VkSampler* s : {&linear_, &nearest_}) {
        if (*s) gpu.device.defer([dev, sampler = *s] { vkDestroySampler(dev, sampler, nullptr); });
        *s = VK_NULL_HANDLE;
    }
}

void SceneProbeGrid::gatherCanopy(SceneFrame& frame) {
    canopy_.clear();
    uint64_t hash = 1469598103934665603ull;
    const auto mix = [&](uint64_t v) {
        hash ^= v;
        hash *= 1099511628211ull;
    };
    float tallest = 0.0f;
    for (SceneNode* node : frame.lists.nodes) {
        if (node->type() != SceneNode::Type::Mesh) continue;
        PlantsNode* plants = static_cast<MeshNode*>(node)->asPlants();
        if (!plants || plants->plantCount() == 0 || plants->speciesCount() == 0) continue;
        const std::vector<float>& data = plants->gpuData();
        const SceneVkBuffer* store = frame.gpu.resources.storage(plants->id(), plants->generation(), data.data(),
                                                                 data.size() * sizeof(float));
        if (!store) continue;
        canopy_.push_back({plants->id(), store->buffer, data.size() * sizeof(float),
                           static_cast<uint32_t>(plants->plantCount())});
        mix(plants->id());
        mix(plants->generation());
        for (int i = 0; i < plants->plantCount(); ++i) {
            const float* p = plants->plant(i);
            const int s = std::clamp(static_cast<int>(p[5]), 0, plants->speciesCount() - 1);
            tallest = std::max(tallest, plants->species(s)[0] * p[4]);
        }
    }
    if (hash != canopyHash_) {
        canopyHash_ = hash;
        ++canopyGeneration_;
        // The layers reach a little over the tallest crown.
        top_ = std::max(8.0f, tallest * 1.15f);
    }
}

void SceneProbeGrid::offer(SceneFrame& frame, WorldService& service) {
    on_ = frame.renderer.probeGridEnabled();
    if (!on_) return;
    rendered_ = true;
    gatherCanopy(frame);
    grid_.setCanopyGeneration(canopyGeneration_);
    grid_.setTop(top_);

    // The sun (the lighting's dominant directional light, toward it).
    const SceneLightingUniforms& light = frame.lighting;
    bromath::Vec3 sun{0.0f, 1.0f, 0.0f};
    if (light.sunDirection[3] > 0.5f) sun = {-light.sunDirection[0], -light.sunDirection[1], -light.sunDirection[2]};
    if (bromath::vdot(sun, sunRef_) < kSunTurnCos) {
        sunRef_ = sun;
        ++sunGeneration_;
    }
    grid_.setSunGeneration(sunGeneration_);

    const bromath::Vec3 eye = frame.view.eye;
    grid_.place(eye.x, eye.z);

    const ProbeGridConfig& config = grid_.config();
    ProbeGridGpu& g = block_;
    g = ProbeGridGpu{};
    g.params[0] = 1.0f;
    g.params[1] = static_cast<float>(std::min(config.cascades, PG_MAX_CASCADES));
    g.params[2] = grid_.top();
    g.params[3] = ProbeGrid::kBottom;
    g.eye[0] = eye.x;
    g.eye[1] = eye.y;
    g.eye[2] = eye.z;
    g.eye[3] = ProbeGrid::kLayerPower;
    g.sun[0] = sunRef_.x;
    g.sun[1] = sunRef_.y;
    g.sun[2] = sunRef_.z;
    for (int c = 0; c < PG_MAX_CASCADES && c < config.cascades; ++c) {
        const double s = grid_.spacing(c);
        // Column i stands in the middle of its world's column i.
        g.cascade[c][0] = static_cast<float>(static_cast<double>(grid_.cornerX(c)) * s - 0.5 * s - eye.x);
        g.cascade[c][1] = static_cast<float>(static_cast<double>(grid_.cornerZ(c)) * s - 0.5 * s - eye.z);
        g.cascade[c][2] = static_cast<float>(s);
        g.cascade[c][3] = static_cast<float>(1.0 / s);
        const auto wrap = [](int64_t v) { return static_cast<float>(((v % PG_COLUMNS) + PG_COLUMNS) % PG_COLUMNS); };
        g.wrap[c][0] = wrap(grid_.cornerX(c));
        g.wrap[c][1] = wrap(grid_.cornerZ(c));
    }
    Block block{};
    block.grid = block_;
    block.capture[0] = kCaptureRange;
    blockInfo_ = frame.gpu.device.frameUniform(&block, sizeof(block));

    // The cascades whose worlds are to be made: one job, required.
    uint32_t stale = 0;
    for (int c = 0; c < config.cascades && c < PG_MAX_CASCADES; ++c)
        if (!grid_.volumesCurrent(c)) ++stale;
    if (stale == 0) return;
    WorldJob job;
    job.key = 0;
    job.kind = kind_;
    job.error = 1.0f;
    job.size = static_cast<double>(stale);
    job.required = true;
    service.offer(job);
}

void SceneProbeGrid::computeBarrier(VkCommandBuffer cmd) {
    render::cmdMemoryBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
}

void SceneProbeGrid::capture(SceneFrame& frame, VkCommandBuffer cmd, uint32_t mask) {
    SceneFrameGraph::transition(cmd, captureDepth_, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth.imageView = captureDepth_.view;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.clearValue.depthStencil = {1.0f, 0};
    VkRenderingInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    info.renderArea = {{0, 0}, {captureDepth_.width, captureDepth_.height}};
    info.layerCount = 1;
    info.pDepthAttachment = &depth;
    frame.gpu.device.cmdBeginRendering(cmd, &info);
    vkCmdSetDepthBias(cmd, 0.0f, 0.0f, 0.0f);

    const bromath::Vec3 eye = frame.view.eye;
    const float R = kCaptureRange;
    for (int c = 0; c < PG_MAX_CASCADES; ++c) {
        if ((mask & (1u << c)) == 0) continue;
        const VkRect2D rect{{0, c * PG_HEIGHT}, {PG_HEIGHT, PG_HEIGHT}};
        const VkViewport viewport{0.0f, static_cast<float>(c * PG_HEIGHT), static_cast<float>(PG_HEIGHT),
                                  static_cast<float>(PG_HEIGHT), 0.0f, 1.0f};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &rect);
        // Straight down onto the cascade's world, camera-relative: x across,
        // z down the rows, the depth from R above the eye (0) to R below (1).
        const float ox = block_.cascade[c][0], oz = block_.cascade[c][1];
        const float W = block_.cascade[c][2] * PG_COLUMNS;
        bromath::Mat4 vp{};
        std::memset(vp.data, 0, sizeof(vp.data));
        vp.data[0] = 2.0f / W;                 // x
        vp.data[12] = -2.0f * ox / W - 1.0f;
        vp.data[9] = 2.0f / W;                 // clip y from z
        vp.data[13] = -2.0f * oz / W - 1.0f;
        vp.data[6] = -0.5f / R;                // depth from y
        vp.data[14] = 0.5f;
        vp.data[15] = 1.0f;

        // The box in the world, for the draws' bounds and chunks.
        const bromath::AABB3 box{{ox + eye.x, eye.y - R, oz + eye.z}, {ox + W + eye.x, eye.y + R, oz + W + eye.z}};
        bromath::Frustum frustum;
        const bromath::Vec3 n[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        const float d[6] = {-box.min.x, box.max.x, -box.min.y, box.max.y, -box.min.z, box.max.z};
        for (int p = 0; p < 6; ++p) frustum.planes[p] = {n[p], d[p]};
        for (const MeshDraw& draw : frame.lists.meshes) {
            if (!groundCaster(draw)) continue;
            if (draw.hasBounds && (draw.bounds.max.x < box.min.x || draw.bounds.min.x > box.max.x ||
                                   draw.bounds.max.z < box.min.z || draw.bounds.min.z > box.max.z))
                continue;
            frame.gpu.casters.draw(frame, cmd, vp, draw, &frustum, SceneCasterDrawer::Mode::Capture);
        }
    }
    frame.gpu.device.cmdEndRendering(cmd);
    SceneFrameGraph::transition(cmd, captureDepth_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void SceneProbeGrid::dispatch(SceneFrame& frame, VkCommandBuffer cmd, Stage stage, uint32_t mask, VkBuffer plants,
                              VkDeviceSize plantsBytes, uint32_t groups) {
    SceneVkDevice& device = frame.gpu.device;
    VkDescriptorSet set = device.frameSet(setLayout_);
    if (set == VK_NULL_HANDLE) return;
    SceneVkDescriptorWriter w;
    w.writeBuffer(0, blockInfo_.buffer, blockInfo_.range, blockInfo_.offset);
    const auto storage = [&](uint32_t binding, const SceneVkImage& img) {
        w.writeImage(binding, img.view, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
    };
    storage(1, height_);
    w.writeImage(2, captureDepth_.view, nearest_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    w.writeBuffer(3, sums_.buffer, sums_.size, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    storage(4, medium_);
    w.writeImage(5, medium_.view, linear_, VK_IMAGE_LAYOUT_GENERAL);
    storage(6, sun_);
    if (plants) w.writeBuffer(7, plants, plantsBytes, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    w.updateSet(device.device(), set);

    const uint32_t job[4] = {mask, 0, 0, 0};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines_[stage]);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(job), job);
    switch (stage) {
    case kHeight: vkCmdDispatch(cmd, PG_HEIGHT / 8, PG_HEIGHT * PG_MAX_CASCADES / 8, 1); break;
    case kSplat: vkCmdDispatch(cmd, groups, 1, 1); break;
    case kResolve:
    case kSun: vkCmdDispatch(cmd, PG_VOXELS / 8, PG_VOXELS / 8, PG_LAYERS_Y * PG_MAX_CASCADES); break;
    case kStages: break;
    }
}

uint32_t SceneProbeGrid::record(SceneFrame& frame, int, std::span<const WorldJob> granted, WorldStream& stream) {
    if (!on_ || granted.empty()) return 0;
    uint32_t mask = 0;
    const int cascades = std::min(grid_.config().cascades, PG_MAX_CASCADES);
    for (int c = 0; c < cascades; ++c)
        if (!grid_.volumesCurrent(c)) mask |= 1u << c;
    if (mask == 0) return 0;
    VkCommandBuffer cmd = stream.cmd();

    // What the frames before read of the world is done with before it is remade.
    render::cmdMemoryBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
    capture(frame, cmd, mask);
    // The cascades' sums cleared for the splat.
    for (int c = 0; c < cascades; ++c)
        if (mask & (1u << c)) vkCmdFillBuffer(cmd, sums_.buffer, kSumBytesPerCascade * c, kSumBytesPerCascade, 0);
    dispatch(frame, cmd, kHeight, mask, VK_NULL_HANDLE, 0, 0);
    computeBarrier(cmd);
    for (const Canopy& node : canopy_) dispatch(frame, cmd, kSplat, mask, node.records, node.bytes, node.plants);
    computeBarrier(cmd);
    dispatch(frame, cmd, kResolve, mask, VK_NULL_HANDLE, 0, 0);
    computeBarrier(cmd);
    dispatch(frame, cmd, kSun, mask, VK_NULL_HANDLE, 0, 0);
    // For the frame's readers (the probes' traces, the lit shaders).
    render::cmdMemoryBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT);
    for (int c = 0; c < cascades; ++c)
        if (mask & (1u << c)) grid_.volumesMade(c);
    return 1;
}

bool SceneProbeGrid::read(SceneGpu& gpu, ProbeGridTables& out) {
    if (!rendered_) return false;
    SceneVkDevice& device = gpu.device;
    device.waitIdle();
    struct Copy {
        const SceneVkImage* image;
        uint32_t texelBytes;
        SceneVkBuffer buffer;
    };
    Copy copies[] = {{&height_, 4, {}}, {&medium_, 8, {}}, {&sun_, 2, {}}};
    VkCommandBuffer cmd = device.frames().beginCommands();
    bool ok = true;
    for (Copy& c : copies) {
        const VkDeviceSize bytes =
            VkDeviceSize{c.image->width} * c.image->height * c.image->depth * c.texelBytes;
        ok = ok && gpu.allocator.createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                              c.buffer) &&
             c.buffer.mappedData;
        if (!ok) break;
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {c.image->width, c.image->height, c.image->depth};
        vkCmdCopyImageToBuffer(cmd, c.image->image, VK_IMAGE_LAYOUT_GENERAL, c.buffer.buffer, 1, &region);
    }
    render::cmdMemoryBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
    const uint64_t ticket = device.frames().submit(cmd);
    ok = ok && ticket != 0 && device.context().queue().wait(ticket);
    if (ok) {
        const auto* h = static_cast<const float*>(copies[0].buffer.mappedData);
        out.height.assign(h, h + VkDeviceSize{height_.width} * height_.height);
        const size_t voxels = size_t{medium_.width} * medium_.height * medium_.depth;
        const auto* m = static_cast<const uint16_t*>(copies[1].buffer.mappedData);
        out.medium.resize(voxels * 4);
        for (size_t i = 0; i < voxels * 4; ++i) out.medium[i] = halfToFloat(m[i]);
        const auto* s = static_cast<const uint16_t*>(copies[2].buffer.mappedData);
        out.sun.resize(voxels);
        for (size_t i = 0; i < voxels; ++i) out.sun[i] = halfToFloat(s[i]);
    }
    for (Copy& c : copies) gpu.allocator.destroyBuffer(c.buffer);
    out.block = block_;
    out.captureRange = kCaptureRange;
    return ok;
}

}  // namespace bro::scene::vk
