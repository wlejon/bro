#include "scene/vulkan/draw/pass_draw_counts.h"

#include "render/vulkan_compute.h"
#include "render/vulkan_util.h"
#include "scene/scene_renderer.h"
#include "scene/vulkan/draw/scene_vk_kernel_hook.h"
#include "scene/vulkan/frame/scene_frame.h"
#include "scene/vulkan/frame/scene_shader_sets.h"
#include "scene/vulkan/frame/scene_vk_device.h"
#include "scene/vulkan/frame/scene_vk_pipeline.h"
#include "scene/vulkan/shadow/scene_shadow_pages.h"
#include "scene/world/shadow_plan.h"
#include "util/log.h"

#include "draw_counts.bk.src.h"
#include "instance_cull.bk.src.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace bro::scene::vk {

namespace {

// draw_counts.bk's DrawCountUBO.
struct DrawCountUniforms {
    uint32_t a[4];   // source word, instance rows, index count, shadow index count
    uint32_t b[4];   // commands word
};

// instance_cull.bk's CullUBO.
struct CullUniforms {
    float planes[6][4];   // relative to the frame's eye: normal, distance
    float model[3][4];    // the draw's model's first three rows
    float s[4];           // x: the model's largest axis scale
    uint32_t a[4];        // count word (kNone), rows, rows' first word, the region's rows' first word
    uint32_t b[4];        // the region's indices' first word, commands word
};
static_assert(sizeof(CullUniforms) == 192);

constexpr uint32_t kNone = 0xffffffffu;
constexpr VkDeviceSize kCommandBytes = sizeof(VkDrawIndexedIndirectCommand);
constexpr uint32_t kCommandWords = kCommandBytes / 4;

VkPipeline computeStage(VkDevice dev, VkPipelineLayout layout, const char* file, const char* src,
                        const char* entry) {
    KernelHookStage stage;
    std::string err;
    if (!compileKernelStage(file, src, entry, stage, err)) {
        LOG_ERROR("PassDrawCounts: %s failed: %s", file, err.c_str());
        return VK_NULL_HANDLE;
    }
    VkShaderModule mod = SceneVkShaderModule::create(dev, stage.spirv);
    VkPipeline p = createComputePipeline(dev, mod, layout, entry, stage.entry.c_str());
    SceneVkShaderModule::destroy(dev, mod);
    return p;
}

VkPipelineLayout pipelineLayout(VkDevice dev, VkDescriptorSetLayout set) {
    VkPipelineLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    li.setLayoutCount = 1;
    li.pSetLayouts = &set;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    return vkCreatePipelineLayout(dev, &li, nullptr, &layout) == VK_SUCCESS ? layout : VK_NULL_HANDLE;
}

}  // namespace

bool PassDrawCounts::setup(SceneGpu& gpu) {
    VkDevice dev = gpu.device.device();
    SceneVkDescriptorLayoutBuilder b;
    b.addBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT);
    b.addBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT);
    b.addBinding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT);
    setLayout_ = b.build(dev);
    if (!setLayout_) return false;
    layout_ = pipelineLayout(dev, setLayout_);
    if (!layout_) return false;
    pipeline_ = computeStage(dev, layout_, "draw/draw_counts.bk", kBkDrawCountsSrc, "draw_counts");
    if (!pipeline_) return false;
    if (!gpu.allocator.createBuffer(kMaxDraws * 2 * kCommandBytes,
                                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, commands_))
        return false;

    SceneVkDescriptorLayoutBuilder c;
    c.addBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT);
    for (uint32_t i = 1; i <= 5; ++i) c.addBinding(i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT);
    cullSetLayout_ = c.build(dev);
    if (!cullSetLayout_) return false;
    cullLayout_ = pipelineLayout(dev, cullSetLayout_);
    if (!cullLayout_) return false;
    cullPipeline_ = computeStage(dev, cullLayout_, "draw/instance_cull.bk", kBkInstanceCullSrc, "instance_cull");
    if (!cullPipeline_) return false;
    views_.reserve(kMaxRegions);
    return gpu.allocator.createBuffer(kMaxRegions * 2 * kCommandBytes,
                                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                          VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, cullCommands_);
}

void PassDrawCounts::cleanup(SceneGpu& gpu) {
    VkDevice dev = gpu.device.device();
    for (VkPipeline p : {pipeline_, cullPipeline_})
        if (p) gpu.device.defer([dev, p] { destroyScenePipeline(dev, p, nullptr); });
    for (VkPipelineLayout l : {layout_, cullLayout_})
        if (l) vkDestroyPipelineLayout(dev, l, nullptr);
    for (VkDescriptorSetLayout l : {setLayout_, cullSetLayout_})
        if (l) vkDestroyDescriptorSetLayout(dev, l, nullptr);
    gpu.allocator.destroyBuffer(commands_);
    gpu.allocator.destroyBuffer(cullCommands_);
    gpu.allocator.destroyBuffer(cullOut_);
    pipeline_ = cullPipeline_ = VK_NULL_HANDLE;
    layout_ = cullLayout_ = VK_NULL_HANDLE;
    setLayout_ = cullSetLayout_ = VK_NULL_HANDLE;
}

bool PassDrawCounts::active(const SceneFrame& frame) const {
    if (!pipeline_ || !commands_.buffer) return false;
    for (const MeshDraw& d : frame.lists.meshes)
        if (d.countBuffer || d.cullSpheres) return true;
    return false;
}

void PassDrawCounts::record(SceneFrame& frame) {
    recordCounts(frame);
    if (cullPipeline_ && cullCommands_.buffer) recordCulls(frame);
}

void PassDrawCounts::recordCounts(SceneFrame& frame) {
    VkCommandBuffer cmd = frame.cmd;
    const VkDeviceSize bytes = kMaxDraws * 2 * kCommandBytes;
    // Last frame's draws read the commands before they are rewritten.
    render::cmdBufferBarrier(cmd, commands_.buffer, 0, bytes, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
                             VK_ACCESS_INDIRECT_COMMAND_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_ACCESS_SHADER_WRITE_BIT);
    bindScenePipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    uint32_t slot = 0;
    for (MeshDraw& d : frame.lists.meshes) {
        if (!d.countBuffer) continue;
        const VkBuffer source = d.countBuffer->buffer();
        if (slot >= kMaxDraws || !source || (VkDeviceSize(d.countWord) + 1) * 4 > d.countBuffer->size()) {
            if (!warned_) {
                LOG_WARN("PassDrawCounts: a counted draw draws all its rows (past %u counted draws, or its word is "
                         "outside its buffer)", kMaxDraws);
                warned_ = true;
            }
            continue;
        }
        DrawCountUniforms u{};
        u.a[0] = d.countWord;
        u.a[1] = d.instanceCount;
        u.a[2] = d.indexCount;
        u.a[3] = d.shadowIndexCount != 0 && d.shadowIndexCount < d.indexCount ? d.shadowIndexCount : d.indexCount;
        u.b[0] = slot * 2 * kCommandWords;
        const VkDescriptorBufferInfo ubo = frame.gpu.device.frameUniform(&u, sizeof(u));
        VkDescriptorSet set = frame.gpu.device.frameSet(setLayout_);
        if (!ubo.buffer || !set) break;
        SceneVkDescriptorWriter w;
        w.writeBuffer(0, ubo.buffer, ubo.range, ubo.offset);
        w.writeBuffer(1, source, d.countBuffer->size(), 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        w.writeBuffer(2, commands_.buffer, bytes, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        w.updateSet(frame.gpu.device.device(), set);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &set, 0, nullptr);
        vkCmdDispatch(cmd, 1, 1, 1);
        d.indirect = commands_.buffer;
        d.indirectOffset = slot * 2 * kCommandBytes;
        ++slot;
    }
    render::cmdBufferBarrier(cmd, commands_.buffer, 0, bytes, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
                             VK_ACCESS_INDIRECT_COMMAND_READ_BIT);
}

void PassDrawCounts::recordCulls(SceneFrame& frame) {
    // The regions: a culled draw's views (0 the camera, 1 + i shadow view i).
    struct Region {
        MeshDraw* draw;
        int view;
        const bromath::Frustum* frustum;
        uint32_t rowsWord, idsWord;
    };
    thread_local std::vector<Region> regions;
    regions.clear();
    views_.clear();
    const ShadowPlan& plan = frame.renderer.shadowPlan();
    const bool shadows = frame.gpu.shadows.ready();
    const bromath::Frustum* camera = frame.renderer.cullingFrustum();
    uint64_t words = 0;
    for (MeshDraw& d : frame.lists.meshes) {
        d.cullViews = nullptr;
        if (!d.cullSpheres || d.kind != MeshKind::Instanced || !d.cullSpheres->buffer()) continue;
        if (views_.size() >= kMaxRegions) break;
        InstanceCullViews& cv = views_.emplace_back();
        const auto add = [&](int view, const bromath::Frustum* f) {
            if (regions.size() >= kMaxRegions) return;
            regions.push_back({&d, view, f, static_cast<uint32_t>(words),
                               static_cast<uint32_t>(words + uint64_t(d.instanceCount) * 16)});
            words += uint64_t(d.instanceCount) * 17;
        };
        if (camera && !d.cameraCulled) add(0, camera);
        if (shadows && d.castsShadow) {
            for (int v = 0; v < plan.viewCount; ++v) {
                const ShadowViewPlan& view = plan.views[v];
                if (!shadowCastsInto(view, d.shadowMotion)) continue;
                if (camera && d.hasBounds && !bromath::fintersects(view.frustum, d.bounds)) continue;
                add(1 + v, &view.frustum);
            }
        }
        d.cullViews = &cv;
    }
    if (regions.empty() || words == 0) {
        for (MeshDraw& d : frame.lists.meshes) d.cullViews = nullptr;
        return;
    }
    if (regions.size() >= kMaxRegions && !cullWarned_) {
        LOG_WARN("PassDrawCounts: past %u culled views a frame, the rest draw their rows whole", kMaxRegions);
        cullWarned_ = true;
    }

    SceneGpu& gpu = frame.gpu;
    const VkDeviceSize outBytes = words * 4;
    if (cullOut_.size < outBytes) {
        gpu.allocator.destroyBuffer(cullOut_);
        if (!gpu.allocator.createBuffer(std::max<VkDeviceSize>(outBytes + outBytes / 2, 1 << 16),
                                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, cullOut_)) {
            for (MeshDraw& d : frame.lists.meshes) d.cullViews = nullptr;
            return;
        }
    }

    VkCommandBuffer cmd = frame.cmd;
    const VkDeviceSize cmdBytes = regions.size() * 2 * kCommandBytes;
    // Last frame's draws read the regions before they are rewritten.
    render::cmdBufferBarrier(cmd, cullCommands_.buffer, 0, cmdBytes, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
                             VK_ACCESS_INDIRECT_COMMAND_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_ACCESS_TRANSFER_WRITE_BIT);
    render::cmdBufferBarrier(cmd, cullOut_.buffer, 0, outBytes, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
                             VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_ACCESS_SHADER_WRITE_BIT);
    // Every region's commands, no instances yet: the cull counts them.
    thread_local std::vector<VkDrawIndexedIndirectCommand> initial;
    initial.assign(regions.size() * 2, VkDrawIndexedIndirectCommand{});
    for (size_t r = 0; r < regions.size(); ++r) {
        const MeshDraw& d = *regions[r].draw;
        initial[2 * r].indexCount = d.indexCount;
        initial[2 * r + 1].indexCount =
            d.shadowIndexCount != 0 && d.shadowIndexCount < d.indexCount ? d.shadowIndexCount : d.indexCount;
    }
    vkCmdUpdateBuffer(cmd, cullCommands_.buffer, 0, cmdBytes, initial.data());
    render::cmdBufferBarrier(cmd, cullCommands_.buffer, 0, cmdBytes, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);

    bindScenePipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cullPipeline_);
    const bromath::Vec3 eye = frame.view.eye;
    for (size_t r = 0; r < regions.size(); ++r) {
        const Region& rg = regions[r];
        MeshDraw& d = *rg.draw;
        CullUniforms u{};
        for (int i = 0; i < 6; ++i) {
            const bromath::Plane& p = rg.frustum->planes[i];
            u.planes[i][0] = p.normal.x;
            u.planes[i][1] = p.normal.y;
            u.planes[i][2] = p.normal.z;
            u.planes[i][3] = p.d + bromath::vdot(p.normal, eye);
        }
        float scale = 0.0f;
        for (int c = 0; c < 3; ++c) {
            const float* col = d.push.model + 4 * c;
            scale = std::max(scale, std::sqrt(col[0] * col[0] + col[1] * col[1] + col[2] * col[2]));
        }
        for (int row = 0; row < 3; ++row)
            for (int c = 0; c < 4; ++c) u.model[row][c] = d.push.model[c * 4 + row];
        u.s[0] = scale;
        const render::ComputeBuffer* spheres = d.cullSpheres;
        const uint32_t rows = static_cast<uint32_t>(std::min<VkDeviceSize>(d.instanceCount, spheres->size() / 16));
        const bool counted = d.countBuffer && d.countBuffer->buffer() &&
                             (VkDeviceSize(d.countWord) + 1) * 4 <= d.countBuffer->size();
        u.a[0] = counted ? d.countWord : kNone;
        u.a[1] = rows;
        u.a[2] = static_cast<uint32_t>(d.instanceOffset / 4);
        u.a[3] = rg.rowsWord;
        u.b[0] = rg.idsWord;
        u.b[1] = static_cast<uint32_t>(r * 2 * kCommandWords);
        const VkDescriptorBufferInfo ubo = gpu.device.frameUniform(&u, sizeof(u));
        VkDescriptorSet set = gpu.device.frameSet(cullSetLayout_);
        if (!ubo.buffer || !set) break;
        SceneVkDescriptorWriter w;
        w.writeBuffer(0, ubo.buffer, ubo.range, ubo.offset);
        if (counted)
            w.writeBuffer(1, d.countBuffer->buffer(), d.countBuffer->size(), 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        else
            w.writeBuffer(1, spheres->buffer(), spheres->size(), 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        w.writeBuffer(2, spheres->buffer(), spheres->size(), 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        w.writeBuffer(3, d.instances, VK_WHOLE_SIZE, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        w.writeBuffer(4, cullOut_.buffer, outBytes, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        w.writeBuffer(5, cullCommands_.buffer, cmdBytes, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        w.updateSet(gpu.device.device(), set);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cullLayout_, 0, 1, &set, 0, nullptr);
        if (rows > 0) vkCmdDispatch(cmd, (rows + 63) / 64, 1, 1);
        // (const: the views table is the pass's, this frame's.)
        auto& cv = const_cast<InstanceCullViews&>(*d.cullViews);
        cv.out = cullOut_.buffer;
        cv.commands = cullCommands_.buffer;
        cv.has[rg.view] = true;
        cv.rows[rg.view] = VkDeviceSize(rg.rowsWord) * 4;
        cv.ids[rg.view] = VkDeviceSize(rg.idsWord) * 4;
        cv.command[rg.view] = r * 2 * kCommandBytes;
    }
    render::cmdBufferBarrier(cmd, cullCommands_.buffer, 0, cmdBytes, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
                             VK_ACCESS_INDIRECT_COMMAND_READ_BIT);
    render::cmdBufferBarrier(cmd, cullOut_.buffer, 0, outBytes, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
                             VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT);
}

}  // namespace bro::scene::vk
