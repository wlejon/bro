#include "scene/vulkan/shadow/scene_shadow_pages.h"

#include "render/vulkan_queue.h"
#include "render/vulkan_util.h"
#include "scene/scene_renderer.h"
#include "scene/vulkan/frame/scene_frame.h"
#include "scene/vulkan/frame/scene_targets.h"
#include "scene/vulkan/frame/scene_vk_device.h"
#include "util/log.h"

#include <algorithm>
#include <cstring>

namespace bro::scene::vk {

bool SceneShadowPages::setup(SceneGpu& gpu) {
    allocator_ = &gpu.allocator;
    // A unit's first guess: a plants page's unit (a texel or an element,
    // ShadowPageWant::units) about what biomes' cost, a still page's what
    // one costs. Both are drawn the next frame (the frame's plan is made
    // before the service runs), so that frame's budget pays for them.
    kind_ = gpu.world.addKind(this, {"shadow pages", WorldSchedule::Raster, 4e-7f, true});
    stillKind_ = gpu.world.addKind(this, {"shadow pages, still", WorldSchedule::Raster, 0.0001f, true});
    // 1x1 until a light casts a shadow: the lighting set always has a pool
    // and a scratch target.
    return kind_ >= 0 && stillKind_ >= 0 && ensureImage(gpu, pool, 1, kPoolUsage, "pool") &&
           ensureImage(gpu, scratch, 1, kScratchUsage, "scratch target");
}

void SceneShadowPages::cleanup(SceneGpu& gpu) {
    gpu.allocator.destroyImage(pool);
    gpu.allocator.destroyImage(scratch);
    gpu.allocator.destroyBuffer(requests_);
    for (Readback& r : ring_) gpu.allocator.destroyBuffer(r.buffer);
}

bool SceneShadowPages::ensureImage(SceneGpu& gpu, SceneVkImage& image, uint32_t side, VkImageUsageFlags usage,
                                   const char* what) {
    if (image.isValid() && image.width == side) return true;
    gpu.allocator.destroyImage(image);
    if (!gpu.allocator.createImage(side, side, SceneTargets::kDepthFormat, usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                   image, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_DEPTH_BIT)) {
        LOG_ERROR("SceneShadowPages: failed creating the %ux%u shadow %s", side, side, what);
        return false;
    }
    return true;
}

bool SceneShadowPages::ensureRequests(SceneGpu& gpu, VkDeviceSize bytes) {
    requestBytes_ = bytes;
    if (requests_.isValid() && requests_.size >= bytes) return true;
    gpu.allocator.destroyBuffer(requests_);
    for (Readback& r : ring_) {
        gpu.allocator.destroyBuffer(r.buffer);
        r = {};
    }
    if (!gpu.allocator.createBuffer(bytes,
                                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, requests_))
        return false;
    for (Readback& r : ring_) {
        if (!gpu.allocator.createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                        r.buffer) ||
            !r.buffer.mappedData)
            return false;
    }
    return true;
}

bool SceneShadowPages::beginFrame(SceneGpu& gpu, SceneRenderer& renderer) {
    const ShadowPlan& plan = renderer.shadowPlan();
    ready_ = false;
    const VkDeviceSize align =
        std::max<VkDeviceSize>(gpu.device.context().deviceProperties().limits.minStorageBufferOffsetAlignment, 16);
    // Without pages the table is one empty entry.
    const auto none = [&] {
        static const uint32_t kNone[4] = {};
        const render::UploadSlice slice = gpu.device.frameUpload(sizeof(kNone), align);
        if (slice) std::memcpy(slice.mapped, kNone, sizeof(kNone));
        table_ = {slice.buffer, slice.offset, sizeof(kNone)};
        return false;
    };
    if (!plan.any() || plan.table.empty()) return none();
    const bool had = pool.width > 1;
    const uint32_t poolSide = static_cast<uint32_t>(std::max(plan.poolColumns, plan.poolRows) * SP_STRIDE);
    const bool poolOk = pool.isValid() && pool.width == poolSide;
    // (A request bit an entry.)
    if (!ensureImage(gpu, pool, poolSide, kPoolUsage, "page pool") ||
        !ensureImage(gpu, scratch, static_cast<uint32_t>(plan.scratchSize), kScratchUsage, "scratch target") ||
        !ensureRequests(gpu, static_cast<VkDeviceSize>((plan.entryCount + 31) / 32) * 4u)) {
        gpu.allocator.destroyImage(pool);
        gpu.allocator.destroyImage(scratch);
        ensureImage(gpu, pool, 1, kPoolUsage, "pool");
        ensureImage(gpu, scratch, 1, kScratchUsage, "scratch target");
        renderer.invalidateShadowPages();
        return none();
    }
    // A new pool holds none of the pages the paging thinks are drawn.
    if (!poolOk && had) renderer.invalidateShadowPages();

    const VkDeviceSize bytes = plan.table.size() * sizeof(uint32_t);
    const render::UploadSlice slice = gpu.device.frameUpload(bytes, align);
    if (!slice) return none();
    std::memcpy(slice.mapped, plan.table.data(), static_cast<size_t>(bytes));
    table_ = {slice.buffer, slice.offset, bytes};
    ready_ = true;
    return true;
}

void SceneShadowPages::recordReadback(VkCommandBuffer cmd, uint64_t serial) {
    Readback& r = ring_[next_];
    next_ = (next_ + 1) % kRing;
    // (A slot still waiting to be read is overwritten: its requests are older.)
    r.serial = serial;
    r.recorded = true;
    r.pending = false;
    render::cmdBufferBarrier(cmd, requests_.buffer, 0, requestBytes_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    const VkBufferCopy region{0, 0, requestBytes_};
    vkCmdCopyBuffer(cmd, requests_.buffer, r.buffer.buffer, 1, &region);
    render::cmdBufferBarrier(cmd, r.buffer.buffer, 0, requestBytes_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
}

void SceneShadowPages::submitted(uint64_t ticket) {
    for (Readback& r : ring_) {
        if (!r.recorded) continue;
        r.recorded = false;
        r.pending = true;
        r.ticket = ticket;
    }
}

void SceneShadowPages::takeRequests(SceneVkDevice& device, ShadowPages& pages) {
    Readback* done[kRing];
    int n = 0;
    for (Readback& r : ring_)
        if (r.pending && device.context().queue().isComplete(r.ticket)) done[n++] = &r;
    std::sort(done, done + n, [](const Readback* a, const Readback* b) { return a->serial < b->serial; });
    for (int i = 0; i < n; ++i) {
        const auto* bits = static_cast<const uint32_t*>(done[i]->buffer.mappedData);
        pages.request(done[i]->serial, std::span<const uint32_t>(bits, static_cast<size_t>(requestBytes_ / 4)));
        done[i]->pending = false;
    }
}

void SceneShadowPages::offer(SceneFrame& frame, WorldService& service) {
    for (const ShadowPageWant& w : frame.renderer.shadowPages().wants()) {
        WorldJob job;
        job.key = w.key;
        job.kind = w.plants ? kind_ : stillKind_;
        job.error = w.error;
        job.size = w.size;
        job.required = w.required;
        job.units = w.units;
        service.offer(job);
    }
}

double SceneShadowPages::record(SceneFrame& frame, int /*kind*/, std::span<const WorldJob> granted,
                                WorldStream& /*stream*/) {
    // Drawn next frame (the shadow pass stamps this kind's span there); each
    // kind's grants are added to the frame's.
    std::vector<uint64_t> keys;
    keys.reserve(granted.size());
    for (const WorldJob& j : granted) keys.push_back(j.key);
    frame.renderer.shadowPages().grant(keys);
    return 0;
}

}  // namespace bro::scene::vk
