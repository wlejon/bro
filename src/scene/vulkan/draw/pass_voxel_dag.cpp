#include "scene/vulkan/draw/pass_voxel_dag.h"

#include "render/vulkan_context.h"
#include "render/vulkan_util.h"
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

// voxeldag_pass.bk VoxelShadowParams.
struct alignas(16) ShadowViewUbo {
    float viewProj[16] = {};
    float invViewProj[16] = {};
    float rect[4] = {};
    float misc[4] = {};   // texel (m), ortho, max steps, 0
};

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

// A pool array the GPU mirrors: the pages written since `sinceGen` go up,
// in runs. A buffer too small is remade with headroom and filled whole, so
// a growing pool remakes it rarely and an edit uploads what it touched.
template <typename T>
bool uploadPagedVec(SceneGpu& gpu, SceneVkBuffer& dst, const bromesh::VoxelDagPool::Paged<T>& src,
                    const std::vector<uint64_t>& pageGens, uint64_t sinceGen) {
    if (src.empty()) return true;    // nothing reads it; the dummy binds while invalid
    constexpr size_t kPage = bromesh::VoxelDagPool::kPageElems;
    const size_t pages = src.pageCount();
    // Pages [p, e) gathered into one staged copy.
    auto upload = [&](size_t p, size_t e) {
        const size_t first = p * kPage;
        const size_t last = std::min(e * kPage, src.size());
        return gpu.allocator.stageAndUploadBuffer(
            dst.buffer, static_cast<VkDeviceSize>((last - first) * sizeof(T)),
            static_cast<VkDeviceSize>(first * sizeof(T)), [&](uint8_t* out) {
                for (size_t q = p; q < e; ++q) {
                    const size_t n = src.pageSize(q) * sizeof(T);
                    std::memcpy(out, src.page(q), n);
                    out += n;
                }
            });
    };
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(src.size() * sizeof(T));
    if (!dst.isValid() || dst.size < bytes) {
        // Outgrown: the old buffer is copied into the new one on the GPU and
        // only the pages written since go up, rather than the whole array
        // (a pool of hundreds of megabytes) staged in one frame.
        SceneVkBuffer old = dst;
        dst = SceneVkBuffer{};
        const VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                         VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        if (!gpu.allocator.createBuffer(bytes + bytes / 2, usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, dst)) {
            dst = old;
            return false;
        }
        if (!old.isValid() || sinceGen == 0) {
            gpu.allocator.destroyBuffer(old);
            return upload(0, pages);
        }
        VkCommandBuffer cmd = gpu.device.uploadCommands();
        render::cmdBufferBarrier(cmd, old.buffer, 0, VK_WHOLE_SIZE,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        const VkBufferCopy whole{0, 0, std::min(old.size, dst.size)};
        vkCmdCopyBuffer(cmd, old.buffer, dst.buffer, 1, &whole);
        render::cmdBufferBarrier(cmd, dst.buffer, 0, VK_WHOLE_SIZE,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        gpu.allocator.destroyBuffer(old);
    }
    const size_t changed = std::min(pageGens.size(), pages);
    for (size_t p = 0; p < changed;) {
        if (pageGens[p] <= sinceGen) { ++p; continue; }
        size_t e = p + 1;
        while (e < changed && pageGens[e] > sinceGen) ++e;
        if (!upload(p, e)) return false;
        p = e;
    }
    return true;
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
    // The shadow casters' view (PassVoxelShadow; the colour pass binds zeros).
    builder.addBinding(9, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
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

    using A = bromesh::VoxelDagPool::Array;
    const uint64_t since = gp.uploadedGen;
    if (uploadPagedVec(gpu, gp.nodes, src.nodes(), src.pageGenerations(A::Nodes), since) &&
        uploadPagedVec(gpu, gp.children, src.children(), src.pageGenerations(A::Children), since) &&
        uploadPagedVec(gpu, gp.bricks, src.bricks(), src.pageGenerations(A::Bricks), since) &&
        uploadPagedVec(gpu, gp.attrs, src.attrs(), src.pageGenerations(A::Attrs), since) &&
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
        const ShadowViewUbo noView{};
        const VkDescriptorBufferInfo viewInfo = device_->frameUniform(&noView, sizeof(noView));
        if (viewInfo.buffer == VK_NULL_HANDLE) continue;
        w.writeBuffer(9, viewInfo.buffer, viewInfo.range, viewInfo.offset);
        w.updateSet(device_->device(), set2);

        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 2, 1, &set2, 0, nullptr);
        // A region's content box an instance (voxeldag_vert: 36 vertices).
        vkCmdDraw(cmd, 36, static_cast<uint32_t>(gpuRegs.size()), 0, 0);
        frame.drewContent = true;
    }
}

bool PassVoxelShadow::setup(SceneGpu& gpu) {
    VkDevice dev = gpu.device.device();
    vs_ = createKernelStageModule(dev, "draw/voxeldag_pass.bk", kBkVoxelDagPassSrc, "voxeldag_shadow_vert");
    fs_ = createKernelStageModule(dev, "draw/voxeldag_pass.bk", kBkVoxelDagPassSrc, "voxeldag_shadow_frag");
    return vs_ != VK_NULL_HANDLE && fs_ != VK_NULL_HANDLE;
}

void PassVoxelShadow::cleanup(SceneGpu& gpu) {
    VkDevice dev = gpu.device.device();
    if (pipeline_) vkDestroyPipeline(dev, pipeline_, nullptr);
    pipeline_ = VK_NULL_HANDLE;
    SceneVkShaderModule::destroy(dev, vs_);
    SceneVkShaderModule::destroy(dev, fs_);
    vs_ = fs_ = VK_NULL_HANDLE;
}

bool PassVoxelShadow::active(const SceneFrame& frame) const {
    if (!frame.gpu.shadows.ready() || !anyRenderableVoxelWorld(frame)) return false;
    const ShadowPlan& plan = frame.renderer.shadowPlan();
    for (int v = 0; v < plan.viewCount; ++v) {
        if (!plan.views[v].dynamic) return true;
    }
    return false;
}

void PassVoxelShadow::declare(const SceneFrame& frame, PassIO& io) const {
    io.depthTarget(frame.gpu.shadows.scratch);
}

void PassVoxelShadow::record(SceneFrame& frame) {
    VkCommandBuffer cmd = frame.cmd;
    const ShadowPlan& plan = frame.renderer.shadowPlan();
    const SceneVkImage& scratch = frame.gpu.shadows.scratch;
    SceneVkDevice& device = *dag_.device_;

    if (!pipeline_) {
        TargetFormat target;
        target.depth = SceneTargets::kDepthFormat;
        SceneVkPipelineBuilder b;
        b.setLabel("voxeldag shadow")
         .setShaderStages(vs_, fs_, "voxeldag_shadow_vert", "voxeldag_shadow_frag")
         .setInputTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
         .setPolygonMode(VK_POLYGON_MODE_FILL)
         .setCullMode(VK_CULL_MODE_NONE)
         .setTarget(target)
         .disableBlending(0)
         .enableDepthTest(true, VK_COMPARE_OP_LESS_OR_EQUAL)
         .setDynamicStates({VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR});
        pipeline_ = b.build(device.device(), dag_.layout_);
        if (!pipeline_) return;
    }

    const double eye[3] = {
        frame.view.origin[0] + static_cast<double>(frame.view.eye.x),
        frame.view.origin[1] + static_cast<double>(frame.view.eye.y),
        frame.view.origin[2] + static_cast<double>(frame.view.eye.z),
    };
    const VkDeviceSize storageAlign = std::max<VkDeviceSize>(
        64, device.context().deviceProperties().limits.minStorageBufferOffsetAlignment);

    // Each world's casting regions, packed about the frame's eye.
    struct World {
        const VoxelWorldNode* node;
        std::vector<GpuVoxelRegion> regions;
    };
    std::vector<World> worlds;
    for (SceneNode* node : frame.lists.nodes) {
        if (node->type() != SceneNode::Type::VoxelWorld) continue;
        const auto* vw = static_cast<const VoxelWorldNode*>(node);
        World w{vw, {}};
        for (const VoxelRegion& reg : vw->regions()) {
            constexpr uint32_t casts = kVoxelRegionEnabled | kVoxelRegionCastShadow;
            if ((reg.flags & casts) != casts || reg.rootNode == bromesh::kDagNullNode) continue;
            w.regions.push_back(reg.packGpu(eye));
        }
        if (!w.regions.empty()) worlds.push_back(std::move(w));
    }
    if (worlds.empty()) return;

    VkRenderingAttachmentInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth.imageView = scratch.view;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    info.renderArea = {{0, 0}, {scratch.width, scratch.height}};
    info.layerCount = 1;
    info.pDepthAttachment = &depth;
    device.cmdBeginRendering(cmd, &info);
    bindScenePipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    const VkDescriptorSet frameSets[2] = {frame.cameraSet, frame.lightingSet};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, dag_.layout_, 0, 2, frameSets, 0, nullptr);

    std::vector<GpuVoxelRegion> inView;
    for (int v = 0; v < plan.viewCount; ++v) {
        const ShadowViewPlan& view = plan.views[v];
        if (view.dynamic) continue;
        const VkRect2D rect{{view.scratchX, view.scratchY},
                            {static_cast<uint32_t>(view.texelsW), static_cast<uint32_t>(view.texelsH)}};
        const VkViewport viewport{static_cast<float>(rect.offset.x), static_cast<float>(rect.offset.y),
                                  static_cast<float>(rect.extent.width), static_cast<float>(rect.extent.height),
                                  0.0f, 1.0f};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &rect);
        const bromath::Mat4 viewProj = toVulkanClip(rebased(view.relViewProj, frame.view.eye - plan.origin));
        ShadowViewUbo ubo{};
        std::memcpy(ubo.viewProj, viewProj.data, sizeof(ubo.viewProj));
        const bromath::Mat4 inv = bromath::minverse(viewProj);
        std::memcpy(ubo.invViewProj, inv.data, sizeof(ubo.invViewProj));
        ubo.rect[0] = viewport.x;
        ubo.rect[1] = viewport.y;
        ubo.rect[2] = viewport.width;
        ubo.rect[3] = viewport.height;
        // A perspective view's texel a metre out: its cells end at the
        // light's near distances' texel, a little fine farther out.
        ubo.misc[0] = view.ortho ? view.texel : (view.texelPerMetre > 0.0f ? 1.0f / view.texelPerMetre : 0.0f);
        ubo.misc[1] = view.ortho ? 1.0f : 0.0f;

        for (const World& w : worlds) {
            // The regions whose content this view's clip box holds.
            inView.clear();
            for (const GpuVoxelRegion& g : w.regions) {
                const float cmin[3] = {(g.content[0] & 0xFFFFu) / 65535.0f, (g.content[0] >> 16u) / 65535.0f,
                                       (g.content[1] & 0xFFFFu) / 65535.0f};
                const float cmax[3] = {(g.content[1] >> 16u) / 65535.0f, (g.content[2] & 0xFFFFu) / 65535.0f,
                                       (g.content[2] >> 16u) / 65535.0f};
                if (cmin[0] > cmax[0] || cmin[1] > cmax[1] || cmin[2] > cmax[2]) continue;
                int outside[4] = {0, 0, 0, 0};
                for (int k = 0; k < 8; ++k) {
                    const float u = ((k & 1) ? cmax[0] : cmin[0]) * g.r0[3];
                    const float vv = ((k & 2) ? cmax[1] : cmin[1]) * g.r0[3];
                    const float ww = ((k & 4) ? cmax[2] : cmin[2]) * g.r0[3];
                    float p[3];
                    for (int a = 0; a < 3; ++a) p[a] = g.r0[a] + g.r1[a] * u + g.r2[a] * vv + g.r3[a] * ww;
                    float c[4];
                    for (int r = 0; r < 4; ++r) {
                        c[r] = viewProj.at(r, 0) * p[0] + viewProj.at(r, 1) * p[1] + viewProj.at(r, 2) * p[2] +
                               viewProj.at(r, 3);
                    }
                    outside[0] += c[0] < -c[3];
                    outside[1] += c[0] > c[3];
                    outside[2] += c[1] < -c[3];
                    outside[3] += c[1] > c[3];
                }
                if (outside[0] == 8 || outside[1] == 8 || outside[2] == 8 || outside[3] == 8) continue;
                inView.push_back(g);
            }
            if (inView.empty()) continue;
            ubo.misc[2] = static_cast<float>(w.node->maxRaySteps());

            const VkDeviceSize regBytes = static_cast<VkDeviceSize>(inView.size() * sizeof(GpuVoxelRegion));
            render::UploadSlice regSlice = device.frameUpload(regBytes, storageAlign);
            if (!regSlice) continue;
            std::memcpy(regSlice.mapped, inView.data(), static_cast<size_t>(regBytes));
            PassVoxelDag::ParamsUbo params{};
            params.counts[0] = static_cast<uint32_t>(inView.size());
            params.counts[1] = w.node->animTick();
            const VkDescriptorBufferInfo paramsInfo = device.frameUniform(&params, sizeof(params));
            const VkDescriptorBufferInfo viewInfo = device.frameUniform(&ubo, sizeof(ubo));
            if (paramsInfo.buffer == VK_NULL_HANDLE || viewInfo.buffer == VK_NULL_HANDLE) continue;

            const PassVoxelDag::GpuPool& gp = dag_.syncNodePool(frame.gpu, w.node->id(), *w.node);
            VkDescriptorSet set2 = device.frameSet(dag_.set2Layout_);
            SceneVkDescriptorWriter wr;
            // (The scene depth is not read by the casters.)
            wr.writeImage(0, frame.gpu.targets.depthSnapshot.view, frame.gpu.targets.depthSnapshot.sampler);
            wr.writeBuffer(1, paramsInfo.buffer, paramsInfo.range, paramsInfo.offset);
            wr.writeBuffer(2, regSlice.buffer, regBytes, regSlice.offset, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
            writeStorageOrDummy(wr, 3, gp.nodes, dag_.dummyBuf_);
            writeStorageOrDummy(wr, 4, gp.children, dag_.dummyBuf_);
            writeStorageOrDummy(wr, 5, gp.bricks, dag_.dummyBuf_);
            writeStorageOrDummy(wr, 6, gp.attrs, dag_.dummyBuf_);
            writeStorageOrDummy(wr, 7, gp.animClips, dag_.dummyBuf_);
            writeStorageOrDummy(wr, 8, gp.animFrames, dag_.dummyBuf_);
            wr.writeBuffer(9, viewInfo.buffer, viewInfo.range, viewInfo.offset);
            wr.updateSet(device.device(), set2);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, dag_.layout_, 2, 1, &set2, 0, nullptr);
            vkCmdDraw(cmd, 36, static_cast<uint32_t>(inView.size()), 0, 0);
        }
    }
    device.cmdEndRendering(cmd);
}

}  // namespace bro::scene::vk
