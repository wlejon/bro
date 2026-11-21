#include "scene/vulkan/draw/pass_voxel_dag.h"

#include "render/vulkan_context.h"
#include "scene/nodes/voxel_world_node.h"
#include "scene/scene_node.h"
#include "scene/vulkan/draw/scene_vk_kernel_hook.h"
#include "scene/vulkan/frame/scene_defaults.h"
#include "scene/vulkan/frame/scene_frame.h"
#include "scene/vulkan/frame/scene_shader_sets.h"
#include "scene/vulkan/frame/scene_targets.h"
#include "scene/vulkan/frame/scene_vk_depth.h"
#include "scene/vulkan/frame/scene_vk_descriptors.h"
#include "scene/vulkan/frame/scene_vk_device.h"
#include "scene/vulkan/frame/scene_vk_pipeline.h"
#include "scene/vulkan/shadow/scene_shadow_pages.h"
#include "voxeldag_pass.bk.src.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace bro::scene::vk {

namespace {

// Whether any of `reg`'s content is inside the view's side planes and within
// its distance band (the band's dithered edges included), and how near the
// content comes to the eye.
bool voxelRegionInView(const VoxelRegion& reg, const double eye[3],
                       const bromath::Mat4& relViewProj, double* nearest) {
    if (reg.contentMin[0] > reg.contentMax[0] || reg.contentMin[1] > reg.contentMax[1] ||
        reg.contentMin[2] > reg.contentMax[2]) {
        return false;
    }
    double origin[3];
    reg.originWorld(origin);
    const double ext = static_cast<double>(reg.extent());
    const float* axes[3] = {reg.right, reg.up, reg.forward};

    // The eye in the region's frame, metres from its origin.
    double local[3];
    for (int k = 0; k < 3; ++k) {
        local[k] = 0.0;
        for (int j = 0; j < 3; ++j) local[k] += (eye[j] - origin[j]) * static_cast<double>(axes[k][j]);
    }
    double dNear = 0.0, dFar = 0.0;
    for (int k = 0; k < 3; ++k) {
        const double lo = static_cast<double>(reg.contentMin[k]) * ext;
        const double hi = static_cast<double>(reg.contentMax[k]) * ext;
        const double out = std::max({lo - local[k], 0.0, local[k] - hi});
        const double far = std::max(std::abs(local[k] - lo), std::abs(local[k] - hi));
        dNear += out * out;
        dFar += far * far;
    }
    dNear = std::sqrt(dNear);
    dFar = std::sqrt(dFar);
    if (reg.maxDist > 0.0f && dNear > static_cast<double>(reg.maxDist)) return false;
    if (reg.minDist > 0.0f && dFar < static_cast<double>(reg.minDist)) return false;

    // Outside one side plane (or behind the eye) with all eight corners: culled.
    int outside[5] = {0, 0, 0, 0, 0};
    for (int c = 0; c < 8; ++c) {
        float p[3];
        for (int j = 0; j < 3; ++j) {
            double w = origin[j] - eye[j];
            for (int k = 0; k < 3; ++k) {
                const float u = ((c >> k) & 1) ? reg.contentMax[k] : reg.contentMin[k];
                w += static_cast<double>(u) * ext * static_cast<double>(axes[k][j]);
            }
            p[j] = static_cast<float>(w);
        }
        float clip[4];
        for (int r = 0; r < 4; ++r) {
            clip[r] = relViewProj.at(r, 0) * p[0] + relViewProj.at(r, 1) * p[1] +
                      relViewProj.at(r, 2) * p[2] + relViewProj.at(r, 3);
        }
        if (clip[0] < -clip[3]) ++outside[0];
        if (clip[0] > clip[3]) ++outside[1];
        if (clip[1] < -clip[3]) ++outside[2];
        if (clip[1] > clip[3]) ++outside[3];
        if (clip[3] <= 0.0f) ++outside[4];
    }
    for (int i = 0; i < 5; ++i) {
        if (outside[i] == 8) return false;
    }
    *nearest = dNear;
    return true;
}

bool anyRenderableVoxelWorld(const SceneFrame& frame) {
    for (const SceneNode* node : frame.lists.nodes) {
        if (node->type() != SceneNode::Type::VoxelWorld) continue;
        if (static_cast<const VoxelWorldNode*>(node)->hasRenderableRegions()) {
            return true;
        }
    }
    return false;
}

template <typename T>
bool uploadStorageVec(SceneGpu& gpu, SceneVkBuffer& dst, const std::vector<T>& src) {
    if (src.empty()) {
        if (dst.isValid()) gpu.allocator.destroyBuffer(dst);
        return true;
    }
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(src.size() * sizeof(T));
    if (!dst.isValid() || dst.size < bytes) {
        gpu.allocator.destroyBuffer(dst);
        const VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (!gpu.allocator.createBuffer(bytes, usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, dst)) {
            return false;
        }
    }
    return gpu.allocator.stageAndUploadBuffer(dst.buffer, src.data(), bytes);
}

void destroyPool(SceneGpu& gpu, auto& pool) {
    gpu.allocator.destroyBuffer(pool.nodes);
    gpu.allocator.destroyBuffer(pool.children);
    gpu.allocator.destroyBuffer(pool.bricks);
    gpu.allocator.destroyBuffer(pool.attrs);
    gpu.allocator.destroyBuffer(pool.animClips);
    gpu.allocator.destroyBuffer(pool.animFrames);
    pool.uploadedGen = 0;
}

void writeStorageOrDummy(SceneVkDescriptorWriter& w, uint32_t binding,
                         const SceneVkBuffer& buf, const SceneVkBuffer& dummy) {
    const SceneVkBuffer& chosen = buf.isValid() ? buf : dummy;
    w.writeBuffer(binding, chosen.buffer, chosen.size, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
}

}  // namespace

bool PassVoxelDagPreDepth::active(const SceneFrame& frame) const {
    return anyRenderableVoxelWorld(frame);
}

void PassVoxelDagPreDepth::declare(const SceneFrame& frame, PassIO& io) const {
    io.transferSrc(frame.gpu.targets.depth);
    io.transferDst(frame.gpu.targets.depthSnapshot);
}

void PassVoxelDagPreDepth::record(SceneFrame& frame) {
    const SceneVkImage& src = frame.gpu.targets.depth;
    const SceneVkImage& dst = frame.gpu.targets.depthSnapshot;
    if (!src.isValid() || !dst.isValid()) return;
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    region.extent = {src.width, src.height, 1};
    vkCmdCopyImage(frame.cmd, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}

bool PassVoxelDag::setup(SceneGpu& gpu) {
    device_ = &gpu.device;
    VkDevice dev = gpu.device.device();

    SceneVkDescriptorLayoutBuilder builder;
    builder.addBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
    builder.addBinding(1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
    // The regions (2) are read by the vertex stage too: it draws their boxes.
    builder.addBinding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
    for (uint32_t b = 3; b <= 8; ++b) {
        builder.addBinding(b, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
    }
    set2Layout_ = builder.build(dev);
    if (!set2Layout_) return false;

    const VkDescriptorSetLayout sets[3] = {gpu.defaults.cameraLayout, gpu.defaults.lightingLayout, set2Layout_};
    VkPipelineLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    info.setLayoutCount = 3;
    info.pSetLayouts = sets;
    if (vkCreatePipelineLayout(dev, &info, nullptr, &layout_) != VK_SUCCESS) return false;

    const VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (!gpu.allocator.createBuffer(64, usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, dummyBuf_)) {
        return false;
    }
    const uint8_t zeros[64] = {};
    gpu.allocator.stageAndUploadBuffer(dummyBuf_.buffer, zeros, sizeof(zeros));

    vs_ = createKernelStageModule(dev, "draw/voxeldag_pass.bk", kBkVoxelDagPassSrc, "voxeldag_vert");
    fs_ = createKernelStageModule(dev, "draw/voxeldag_pass.bk", kBkVoxelDagPassSrc, "voxeldag_frag");
    fsIndirect_ = createKernelStageModule(dev, "draw/voxeldag_pass.bk", kBkVoxelDagPassSrc, "voxeldag_indirect_frag");
    return vs_ != VK_NULL_HANDLE && fs_ != VK_NULL_HANDLE && fsIndirect_ != VK_NULL_HANDLE;
}

void PassVoxelDag::cleanup(SceneGpu& gpu) {
    VkDevice dev = gpu.device.device();
    pipelines_.destroy(gpu.device);
    for (auto& [_, pool] : nodePools_) {
        destroyPool(gpu, pool);
    }
    nodePools_.clear();
    gpu.allocator.destroyBuffer(dummyBuf_);
    SceneVkShaderModule::destroy(dev, vs_);
    SceneVkShaderModule::destroy(dev, fs_);
    SceneVkShaderModule::destroy(dev, fsIndirect_);
    vs_ = fs_ = fsIndirect_ = VK_NULL_HANDLE;
    if (layout_) vkDestroyPipelineLayout(dev, layout_, nullptr);
    if (set2Layout_) vkDestroyDescriptorSetLayout(dev, set2Layout_, nullptr);
    layout_ = VK_NULL_HANDLE;
    set2Layout_ = VK_NULL_HANDLE;
}

void PassVoxelDag::releaseNodes(SceneGpu& gpu, std::span<const uint32_t> ids) {
    for (uint32_t id : ids) {
        auto it = nodePools_.find(id);
        if (it == nodePools_.end()) continue;
        destroyPool(gpu, it->second);
        nodePools_.erase(it);
    }
}

bool PassVoxelDag::active(const SceneFrame& frame) const {
    return anyRenderableVoxelWorld(frame);
}

void PassVoxelDag::declare(const SceneFrame& frame, PassIO& io) const {
    io.hdr({.indirect = frame.ssao});
    io.sample(frame.gpu.targets.depthSnapshot);
    io.sample(frame.gpu.shadows.pool);
    io.sample(frame.gpu.shadows.scratch);
}

PassVoxelDag::GpuPool& PassVoxelDag::syncNodePool(
    SceneGpu& gpu, uint32_t nodeId, const bro::scene::VoxelWorldNode& node) {
    GpuPool& gp = nodePools_[nodeId];
    const auto& src = node.pool();
    const uint64_t gen = src.generation();
    if (gp.uploadedGen == gen) return gp;

    if (uploadStorageVec(gpu, gp.nodes, src.nodes()) &&
        uploadStorageVec(gpu, gp.children, src.children()) &&
        uploadStorageVec(gpu, gp.bricks, src.bricks()) &&
        uploadStorageVec(gpu, gp.attrs, src.attrs()) &&
        uploadStorageVec(gpu, gp.animClips, src.animClips()) &&
        uploadStorageVec(gpu, gp.animFrames, src.animFrames())) {
        gp.uploadedGen = gen;
    }
    return gp;
}

void PassVoxelDag::record(SceneFrame& frame) {
    std::vector<const VoxelWorldNode*> worlds;
    for (SceneNode* node : frame.lists.nodes) {
        if (node->type() != SceneNode::Type::VoxelWorld) continue;
        const auto* vw = static_cast<const VoxelWorldNode*>(node);
        if (vw->hasRenderableRegions()) worlds.push_back(vw);
    }
    if (worlds.empty()) return;

    const bool hasIndirect = frame.hdrTarget.colorCount > 1;
    const uint64_t variant = hasIndirect ? 1u : 0u;
    VkShaderModule chosenFs = hasIndirect ? fsIndirect_ : fs_;
    const char* fsEntry = hasIndirect ? "voxeldag_indirect_frag" : "voxeldag_frag";

    VkPipeline pipeline = pipelines_.get(variant, frame.hdrTarget, [&] {
        SceneVkPipelineBuilder b;
        b.setLabel("voxeldag")
         .setShaderStages(vs_, chosenFs, "voxeldag_vert", fsEntry)
         .setInputTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
         .setPolygonMode(VK_POLYGON_MODE_FILL)
         .setCullMode(VK_CULL_MODE_NONE)
         .setTarget(frame.hdrTarget)
         .disableBlending(frame.hdrTarget.colorCount)
         .enableDepthTest(true, depth::compareCloser());
        return b.build(device_->device(), layout_);
    });
    if (!pipeline) return;

    VkCommandBuffer cmd = frame.cmd;
    bindScenePipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    const VkDescriptorSet frameSets[2] = {frame.cameraSet, frame.lightingSet};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 2, frameSets, 0, nullptr);

    const SceneVkImage& depth = frame.gpu.targets.depthSnapshot;
    const VkDeviceSize storageAlign = std::max<VkDeviceSize>(
        64, device_->context().deviceProperties().limits.minStorageBufferOffsetAlignment);
    const double eye[3] = {
        frame.view.origin[0] + static_cast<double>(frame.view.eye.x),
        frame.view.origin[1] + static_cast<double>(frame.view.eye.y),
        frame.view.origin[2] + static_cast<double>(frame.view.eye.z),
    };
    const float projY = std::max(std::abs(frame.view.proj.at(1, 1)), 1e-4f);
    const float rows = static_cast<float>(std::max(1u, frame.gpu.targets.height()));
    const float pixelCone = 2.0f / (projY * rows);

    for (const VoxelWorldNode* vw : worlds) {
        // A region is drawn as its content box, only when some of its
        // content is in view and in its distance band, nearest first.
        std::vector<std::pair<double, GpuVoxelRegion>> visible;
        visible.reserve(vw->regions().size());
        for (const VoxelRegion& reg : vw->regions()) {
            if ((reg.flags & kVoxelRegionEnabled) == 0u || reg.rootNode == bromesh::kDagNullNode) continue;
            double nearest = 0.0;
            if (!voxelRegionInView(reg, eye, frame.view.relViewProj, &nearest)) continue;
            visible.emplace_back(nearest, reg.packGpu(eye));
        }
        if (visible.empty()) continue;
        std::sort(visible.begin(), visible.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        std::vector<GpuVoxelRegion> gpuRegs;
        gpuRegs.reserve(visible.size());
        for (const auto& v : visible) gpuRegs.push_back(v.second);

        const VkDeviceSize regBytes = static_cast<VkDeviceSize>(gpuRegs.size() * sizeof(GpuVoxelRegion));
        render::UploadSlice regSlice = device_->frameUpload(regBytes, storageAlign);
        if (!regSlice) continue;
        std::memcpy(regSlice.mapped, gpuRegs.data(), static_cast<size_t>(regBytes));

        ParamsUbo params{};
        params.counts[0] = static_cast<uint32_t>(gpuRegs.size());
        params.counts[1] = vw->animTick();
        params.counts[2] = vw->maxRaySteps();
        params.counts[3] = vw->shadowSteps();
        params.scales[0] = vw->lodScale();
        params.scales[1] = pixelCone;
        const VkDescriptorBufferInfo uboInfo = device_->frameUniform(&params, sizeof(params));
        if (uboInfo.buffer == VK_NULL_HANDLE) continue;

        const GpuPool& gp = syncNodePool(frame.gpu, vw->id(), *vw);

        VkDescriptorSet set2 = device_->frameSet(set2Layout_);
        SceneVkDescriptorWriter w;
        w.writeImage(0, depth.view, depth.sampler);
        w.writeBuffer(1, uboInfo.buffer, uboInfo.range, uboInfo.offset);
        w.writeBuffer(2, regSlice.buffer, regBytes, regSlice.offset, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        writeStorageOrDummy(w, 3, gp.nodes, dummyBuf_);
        writeStorageOrDummy(w, 4, gp.children, dummyBuf_);
        writeStorageOrDummy(w, 5, gp.bricks, dummyBuf_);
        writeStorageOrDummy(w, 6, gp.attrs, dummyBuf_);
        writeStorageOrDummy(w, 7, gp.animClips, dummyBuf_);
        writeStorageOrDummy(w, 8, gp.animFrames, dummyBuf_);
        w.updateSet(device_->device(), set2);

        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 2, 1, &set2, 0, nullptr);
        // A region's content box an instance (voxeldag_vert: 36 vertices).
        vkCmdDraw(cmd, 36, static_cast<uint32_t>(gpuRegs.size()), 0, 0);
        frame.drewContent = true;
    }
}

}  // namespace bro::scene::vk
