// SkiaGpu's uploads ahead of the first draw (gpu_image_upload.h): the
// uploader thread, the copy + mip chain recorded per upload, the borrowed Skia
// image, and the registry a painter finds a preloaded picture's upload in.

#include "render/gpu_image_upload.h"
#include "render/skia_gpu.h"
#include "render/vulkan_util.h"
#include "util/log.h"
#include "util/main_loop_wake.h"

#include <include/core/SkColorSpace.h>
#include <include/gpu/ganesh/GrBackendSurface.h>
#include <include/gpu/ganesh/GrDirectContext.h>
#include <include/gpu/ganesh/SkImageGanesh.h>
#include <include/gpu/ganesh/vk/GrVkBackendSurface.h>
#include <include/gpu/ganesh/vk/GrVkTypes.h>

#include <algorithm>
#include <chrono>
#include <cstring>

namespace bro::render {

namespace {

constexpr VkFormat kUploadFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkImageUsageFlags kUploadUsage =
    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
// A preloaded picture nobody painted for this long stops being held for one.
constexpr uint64_t kUnclaimedHoldMs = 10000;
// Without a queue apart from graphics, a copy bigger than this goes in bands
// of rows no bigger, each its own submission: a frame submitted meanwhile
// waits for one band, not the whole copy (MoltenVK, M2 Pro: a 24 MP copy and
// its mips held the queue 6-7 ms).
constexpr VkDeviceSize kCopyBandBytes = 16u << 20;

std::mutex gTargetMutex;
SkiaGpu* gTarget = nullptr;  // guarded by gTargetMutex

uint64_t nowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

void releaseUploadRef(void* ctx) {
    delete static_cast<SkiaImageRef*>(ctx);
}

uint32_t mipLevelsFor(int w, int h) {
    uint32_t levels = 1;
    for (int m = std::max(w, h); m > 1; m >>= 1) ++levels;
    return levels;
}

}  // namespace

// ---------------------------------------------------------------------------
// GpuImageUpload
// ---------------------------------------------------------------------------

GpuImageUpload::~GpuImageUpload() {
    if (image_ && gpu_) {
        // The borrowed image's last unref reaches the Skia context.
        SkiaGpu::Lock lock(*gpu_);
        image_.reset();
    }
    image_.reset();
    backend_.reset();
    // vkImage_ goes with its last holder (Skia's release proc may still hold
    // one), which retires it to the SkiaGpu.
}

bool GpuImageUpload::ensureSubmitted() {
    State s = state();
    if (s == State::Submitted) return true;
    if (s == State::Failed || !gpu_) return false;
    if (s == State::Queued) {
        State expected = State::Queued;
        if (state_.compare_exchange_strong(expected, State::Writing, std::memory_order_acq_rel)) {
            gpu_->runUpload(*this);
            return submitted();
        }
    }
    std::unique_lock<std::mutex> lk(mu_);
    cv_.wait(lk, [&] {
        const State st = state();
        return st == State::Submitted || st == State::Failed;
    });
    return submitted();
}

sk_sp<SkImage> GpuImageUpload::image(SkiaGpu& gpu) {
    if (&gpu != gpu_ || !submitted() || !gpu.context()) return nullptr;
    if (image_) return image_;
    if (!backend_) {
        GrVkImageInfo info;
        info.fImage = vkImage_->image;
        info.fAlloc.fMemory = memory_;
        info.fAlloc.fOffset = memoryOffset_;
        info.fAlloc.fSize = memorySize_;
        info.fImageTiling = VK_IMAGE_TILING_OPTIMAL;
        // Where the upload left every level: Skia's first use records the
        // transition out of it, ordered after the copy (see the header).
        info.fImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        info.fFormat = kUploadFormat;
        info.fImageUsageFlags = kUploadUsage;
        info.fSampleCount = 1;
        info.fLevelCount = levels_;
        info.fCurrentQueueFamily = VK_QUEUE_FAMILY_IGNORED;
        backend_ = std::make_unique<GrBackendTexture>(GrBackendTextures::MakeVk(width_, height_, info));
    }
    image_ = SkImages::BorrowTextureFrom(gpu.context(), *backend_, kTopLeft_GrSurfaceOrigin, kRGBA_8888_SkColorType,
                                         kUnpremul_SkAlphaType, nullptr, releaseUploadRef,
                                         new SkiaImageRef(vkImage_));
    if (!image_) LOG_ERROR("SkiaGpu: Skia could not borrow a %dx%d uploaded texture", width_, height_);
    return image_;
}

// ---------------------------------------------------------------------------
// SkiaGpu: the upload target
// ---------------------------------------------------------------------------

void SkiaGpu::becomeUploadTarget(SkiaGpu* gpu) {
    std::lock_guard<std::mutex> lock(gTargetMutex);
    if (!gTarget) gTarget = gpu;
}

std::shared_ptr<GpuImageUpload> SkiaGpu::uploadToTarget(const SharedPixels& px) {
    std::lock_guard<std::mutex> lock(gTargetMutex);
    return gTarget ? gTarget->uploadImage(px) : nullptr;
}

void SkiaGpu::preloadOnTarget(const SharedPixels& px) {
    std::lock_guard<std::mutex> lock(gTargetMutex);
    if (gTarget) gTarget->preloadImage(px);
}

std::shared_ptr<GpuImageUpload> SkiaGpu::findOnTarget(uint64_t pixelsId) {
    std::lock_guard<std::mutex> lock(gTargetMutex);
    return gTarget ? gTarget->findUpload(pixelsId, /*claim=*/false) : nullptr;
}

SkiaGpu::MemoryStats SkiaGpu::targetMemoryStats() {
    MemoryStats m;
    // Not under gTargetMutex while taking the Skia lock: a draw holding the
    // lock may look an upload up on the target (findOnTarget). The target is
    // the engine's SkiaGpu, alive while a page can ask.
    SkiaGpu* gpu = nullptr;
    {
        std::lock_guard<std::mutex> targetLock(gTargetMutex);
        gpu = gTarget;
    }
    if (!gpu) return m;
    m.valid = true;
    if (gpu->context_) {
        Lock lock(*gpu);
        gpu->context_->getResourceCacheUsage(&m.ganeshCount, &m.ganeshBytes);
        m.ganeshPurgeableBytes = gpu->context_->getResourceCachePurgeableBytes();
        m.ganeshLimit = gpu->context_->getResourceCacheLimit();
        if (gpu->memoryAllocator_) {
            const auto [allocated, used] = gpu->memoryAllocator_->totalAllocatedAndUsedMemory();
            m.ganeshVmaAllocated = static_cast<size_t>(allocated);
            m.ganeshVmaUsed = static_cast<size_t>(used);
        }
    }
    const VulkanAllocatorStats s = gpu->vulkan_.memoryPool().stats();
    m.poolHostVisibleBytes = s.hostVisibleBytes;
    m.poolDeviceOnlyBytes = s.deviceOnlyBytes;
    m.poolAllocations = s.activeAllocationCount;
    m.poolBlocks = s.activeBlockCount;
    m.poolDedicated = s.dedicatedAllocationCount;
    m.uploadsLive = static_cast<size_t>(gpu->liveImages_.load(std::memory_order_relaxed));
    m.uploadCopyQueue = gpu->vulkan_.uploadQueue() != nullptr;
    return m;
}

// ---------------------------------------------------------------------------
// SkiaGpu: queueing and the registry
// ---------------------------------------------------------------------------

std::shared_ptr<GpuImageUpload> SkiaGpu::uploadImage(const SharedPixels& px) {
    if (!context_ || !px.rgba || px.width <= 0 || px.height <= 0) return nullptr;
    std::shared_ptr<GpuImageUpload> up(new GpuImageUpload());
    up->gpu_ = this;
    up->px_ = px;
    up->width_ = px.width;
    up->height_ = px.height;
    up->pixelsId_ = px.id;
    {
        std::lock_guard<std::mutex> lock(uploadMutex_);
        if (uploadStop_) return nullptr;
        if (!uploader_.joinable()) uploader_ = std::thread([this] { uploaderLoop(); });
        uploadQueue_.push_back(up);
        std::erase_if(uploads_, [](const std::weak_ptr<GpuImageUpload>& w) { return w.expired(); });
        uploads_.push_back(up);
    }
    uploadCv_.notify_one();
    return up;
}

void SkiaGpu::preloadImage(const SharedPixels& px) {
    if (px.id == 0) return;
    {
        std::lock_guard<std::mutex> lock(uploadMutex_);
        auto it = published_.find(px.id);
        if (it != published_.end() && !it->second.upload.expired()) return;
    }
    std::shared_ptr<GpuImageUpload> up = uploadImage(px);
    if (!up) return;
    std::lock_guard<std::mutex> lock(uploadMutex_);
    // Prune: pictures that died, uploads nobody holds, holds gone stale.
    const uint64_t now = nowMs();
    for (auto it = published_.begin(); it != published_.end();) {
        PublishedUpload& p = it->second;
        if (p.hold && now - p.sinceMs > kUnclaimedHoldMs) p.hold.reset();
        if (p.owner.expired() || p.upload.expired()) it = published_.erase(it);
        else ++it;
    }
    published_[px.id] = PublishedUpload{px.owner, up, up, now};
}

std::shared_ptr<GpuImageUpload> SkiaGpu::findUpload(uint64_t pixelsId, bool claim) {
    if (pixelsId == 0) return nullptr;
    std::shared_ptr<GpuImageUpload> up;
    std::shared_ptr<GpuImageUpload> dropped;  // released outside the mutex
    {
        std::lock_guard<std::mutex> lock(uploadMutex_);
        auto it = published_.find(pixelsId);
        if (it == published_.end()) return nullptr;
        up = it->second.upload.lock();
        if (!up || it->second.owner.expired()) {
            dropped = std::move(it->second.hold);
            published_.erase(it);
            return nullptr;
        }
        if (claim) dropped = std::move(it->second.hold);
    }
    return up;
}

// ---------------------------------------------------------------------------
// SkiaGpu: the uploader thread and the upload itself
// ---------------------------------------------------------------------------

void SkiaGpu::uploaderLoop() {
    for (;;) {
        std::shared_ptr<GpuImageUpload> job;
        uint64_t oldestCopy = 0;
        {
            std::unique_lock<std::mutex> lk(uploadMutex_);
            uploadCv_.wait(lk, [&] { return uploadStop_ || !uploadQueue_.empty() || !uploadStaging_.empty(); });
            if (uploadStop_) return;
            while (!job && !uploadQueue_.empty()) {
                job = uploadQueue_.front().lock();
                uploadQueue_.pop_front();
            }
            if (!job && !uploadStaging_.empty()) oldestCopy = uploadStaging_.front().ticket;
        }
        if (job) {
            GpuImageUpload::State expected = GpuImageUpload::State::Queued;
            if (job->state_.compare_exchange_strong(expected, GpuImageUpload::State::Writing,
                                                    std::memory_order_acq_rel)) {
                runUpload(*job);
            }
            job.reset();
            freeUploadStaging(false);
            continue;
        }
        if (oldestCopy) {
            // Nothing queued and a copy in flight: its staging buffer (as big
            // as the picture) goes as soon as the copy completes. Waited in
            // short slices so a new upload is not held up behind it.
            vulkan_.queue().wait(oldestCopy, 4'000'000);
            freeUploadStaging(false);
        }
    }
}

void SkiaGpu::runUpload(GpuImageUpload& up) {
    uploadsRunning_.fetch_add(1, std::memory_order_acq_rel);
    SharedPixels px = std::move(up.px_);
    up.px_ = SharedPixels{};
    VkDevice device = vulkan_.device();

    auto settle = [&](GpuImageUpload::State s) {
        {
            std::lock_guard<std::mutex> lk(up.mu_);
            up.state_.store(s, std::memory_order_release);
        }
        up.cv_.notify_all();
        uploadsRunning_.fetch_sub(1, std::memory_order_acq_rel);
    };

    const uint32_t w = static_cast<uint32_t>(px.width);
    const uint32_t h = static_cast<uint32_t>(px.height);
    const uint32_t maxDim = vulkan_.deviceProperties().limits.maxImageDimension2D;
    if (!px.rgba || w == 0 || h == 0 || w > maxDim || h > maxDim) {
        settle(GpuImageUpload::State::Failed);
        return;
    }
    const uint32_t levels = mipLevelsFor(px.width, px.height);
    const VkDeviceSize bytes = VkDeviceSize(w) * h * 4;

    // The texture.
    auto img = std::make_shared<SkiaImage>();
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    if (!vulkan_.createImage(w, h, kUploadFormat, VK_IMAGE_TILING_OPTIMAL, kUploadUsage,
                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, img->image, memory, offset, img->allocId_, levels)) {
        LOG_ERROR("SkiaGpu: no %ux%u texture for an upload", w, h);
        settle(GpuImageUpload::State::Failed);
        return;
    }
    img->gpu_ = this;  // from here on its destructor retires it
    liveImages_.fetch_add(1, std::memory_order_relaxed);
    img->format = kUploadFormat;
    img->width = w;
    img->height = h;
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device, img->image, &req);

    // The staging buffer, written here: the one copy of the pixels this path
    // makes, off the page thread.
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    VkDeviceSize stagingOffset = 0;
    uint64_t stagingAlloc = 0;
    void* mapped = nullptr;
    if (!vulkan_.createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging,
                              stagingMemory, stagingOffset, stagingAlloc, mapped) ||
        !mapped) {
        LOG_ERROR("SkiaGpu: no %llu-byte staging buffer for an upload", static_cast<unsigned long long>(bytes));
        if (staging != VK_NULL_HANDLE) vulkan_.destroyBuffer(staging, stagingAlloc);
        settle(GpuImageUpload::State::Failed);
        return;
    }
    std::memcpy(mapped, px.rgba, static_cast<size_t>(bytes));
    px = SharedPixels{};  // the owner may go now

    // The copy, then the mip chain. Where the device has a transfer family
    // apart from graphics (a discrete GPU's copy engine), the copy runs on it:
    // over PCIe a 24 MP copy is tens of ms of GPU time (60-130 ms on a x4
    // link, or once an idle GPU's link has trained down), and on the graphics
    // queue every frame submitted after it would
    // finish after it, so the frame ring's wait for a frame slot two frames
    // later waited for the copy. The uploader waits for it here instead, then
    // gives the image to the graphics family for the mip blits (blits need a
    // graphics queue; a few ms of VRAM-to-VRAM work). Without such a family
    // (unified memory, where the copy is short) both are one graphics
    // submission.
    VulkanQueue& gfxQueue = vulkan_.queue();
    VulkanQueue* copyQueue = vulkan_.uploadQueue();
    const uint32_t gfxFamily = static_cast<uint32_t>(vulkan_.queueFamilies().graphicsFamily);
    const uint32_t copyFamily =
        copyQueue ? static_cast<uint32_t>(vulkan_.queueFamilies().transferFamily) : gfxFamily;
    const VkImage image = img->image;

    // A command buffer in a transient pool of its own, begun.
    auto beginCommands = [&](uint32_t family, VkCommandPool& pool) -> VkCommandBuffer {
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        poolInfo.queueFamilyIndex = family;
        if (vkCreateCommandPool(device, &poolInfo, nullptr, &pool) != VK_SUCCESS) {
            pool = VK_NULL_HANDLE;
            return VK_NULL_HANDLE;
        }
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        allocInfo.commandPool = pool;
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        if (vkAllocateCommandBuffers(device, &allocInfo, &cmd) != VK_SUCCESS ||
            vkBeginCommandBuffer(cmd, &begin) != VK_SUCCESS)
            return VK_NULL_HANDLE;
        return cmd;
    };
    // Every level between the copy queue and the graphics queue, in
    // TRANSFER_DST: recorded once on each side.
    auto ownership = [&](VkCommandBuffer cmd, bool release) {
        ImageBarrier b;
        b.image = image;
        b.range = colorRange(levels);
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamily = copyFamily;
        b.dstQueueFamily = gfxFamily;
        if (release) {
            b.srcStages = VK_PIPELINE_STAGE_TRANSFER_BIT;
            b.srcAccess = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.dstStages = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
        } else {
            b.srcStages = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            b.dstStages = VK_PIPELINE_STAGE_TRANSFER_BIT;
            b.dstAccess = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        }
        cmdImageBarrier(cmd, b);
    };

    // The copy.
    VkCommandPool copyPool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = beginCommands(copyFamily, copyPool);
    bool recorded = cmd != VK_NULL_HANDLE;
    if (recorded) {
        ImageBarrier toDst;
        toDst.image = image;
        toDst.range = colorRange(levels);
        toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toDst.srcStages = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        toDst.dstStages = VK_PIPELINE_STAGE_TRANSFER_BIT;
        toDst.dstAccess = VK_ACCESS_TRANSFER_WRITE_BIT;
        cmdImageBarrier(cmd, toDst);

        auto copyRows = [&](VkCommandBuffer c, uint32_t row0, uint32_t rows) {
            VkBufferImageCopy region{};
            region.bufferOffset = VkDeviceSize(row0) * w * 4;
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageOffset = {0, static_cast<int32_t>(row0), 0};
            region.imageExtent = {w, rows, 1};
            vkCmdCopyBufferToImage(c, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        };
        // On the graphics queue, all but the last band each go in a
        // submission of their own, waited for here, so the frames submitted
        // meanwhile run between them; the last goes with the mip chain. (On
        // one queue, submission order orders the bands' writes before the
        // blits' barrier.)
        uint32_t row = 0;
        if (!copyQueue && bytes > kCopyBandBytes) {
            const uint32_t bandRows = std::max<uint32_t>(1, static_cast<uint32_t>(kCopyBandBytes / (VkDeviceSize(w) * 4)));
            while (recorded && h - row > bandRows) {
                copyRows(cmd, row, bandRows);
                row += bandRows;
                const uint64_t band = vkEndCommandBuffer(cmd) == VK_SUCCESS
                                          ? gfxQueue.submit(QueueSubmit{{cmd}, {}, {}})
                                          : 0;
                const bool done = band != 0 && gfxQueue.wait(band);
                if (band != 0 && !done) gfxQueue.waitIdle();  // the pool is reset below
                VkCommandBufferBeginInfo begin{};
                begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                recorded = done && vkResetCommandPool(device, copyPool, 0) == VK_SUCCESS &&
                           vkBeginCommandBuffer(cmd, &begin) == VK_SUCCESS;
            }
        }
        if (recorded) copyRows(cmd, row, h - row);
        if (copyQueue) {
            ownership(cmd, /*release=*/true);
            recorded = vkEndCommandBuffer(cmd) == VK_SUCCESS;
        }
    }

    // On the upload queue: submit the copy and wait for it, on this thread.
    // The staging buffer and the copy's pool go as soon as it is done.
    SemaphoreWait copyDone;
    VkCommandPool gfxPool = copyPool;
    if (recorded && copyQueue) {
        const uint64_t copyTicket = copyQueue->submit(QueueSubmit{{cmd}, {}, {}});
        const bool copied = copyTicket != 0 && copyQueue->wait(copyTicket);
        if (copyTicket != 0 && !copied) copyQueue->waitIdle();  // nothing may still read what goes below
        vkDestroyCommandPool(device, copyPool, nullptr);
        vulkan_.destroyBuffer(staging, stagingAlloc);
        staging = VK_NULL_HANDLE;
        stagingAlloc = 0;
        if (!copied) {
            LOG_ERROR("SkiaGpu: an upload's copy could not be submitted or did not complete");
            settle(GpuImageUpload::State::Failed);
            return;  // img retires with its last reference
        }
        // Already signalled: orders the acquire after the release for the
        // queues (and validation) without the graphics queue waiting.
        copyDone.semaphore = copyQueue->timeline();
        copyDone.stages = VK_PIPELINE_STAGE_TRANSFER_BIT;
        copyDone.value = copyTicket;
        cmd = beginCommands(gfxFamily, gfxPool);
        recorded = cmd != VK_NULL_HANDLE;
        if (recorded) ownership(cmd, /*release=*/false);
    }

    // The mip chain, on the graphics queue.
    if (recorded) {
        // The mip chain on the GPU: each level a linear blit of the one above
        // (the CPU chain Skia builds for a raster image costs a 24 MP photo
        // ~100 ms; this is a millisecond or two of GPU time).
        int32_t mw = static_cast<int32_t>(w), mh = static_cast<int32_t>(h);
        for (uint32_t level = 1; level < levels; ++level) {
            ImageBarrier toSrc;
            toSrc.image = image;
            toSrc.range = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 1, 0, 1};
            toSrc.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            toSrc.srcStages = VK_PIPELINE_STAGE_TRANSFER_BIT;
            toSrc.srcAccess = VK_ACCESS_TRANSFER_WRITE_BIT;
            toSrc.dstStages = VK_PIPELINE_STAGE_TRANSFER_BIT;
            toSrc.dstAccess = VK_ACCESS_TRANSFER_READ_BIT;
            cmdImageBarrier(cmd, toSrc);
            const int32_t nw = std::max(1, mw / 2), nh = std::max(1, mh / 2);
            VkImageBlit blit{};
            blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1};
            blit.srcOffsets[1] = {mw, mh, 1};
            blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
            blit.dstOffsets[1] = {nw, nh, 1};
            vkCmdBlitImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
            mw = nw;
            mh = nh;
        }
        if (levels > 1) {
            // Every level back in TRANSFER_DST: one layout for the whole image,
            // the one Skia is told it is in.
            ImageBarrier back;
            back.image = image;
            back.range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels - 1, 0, 1};
            back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            back.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            back.srcStages = VK_PIPELINE_STAGE_TRANSFER_BIT;
            back.srcAccess = 0;
            back.dstStages = VK_PIPELINE_STAGE_TRANSFER_BIT;
            back.dstAccess = VK_ACCESS_TRANSFER_WRITE_BIT;
            cmdImageBarrier(cmd, back);
        }
        recorded = vkEndCommandBuffer(cmd) == VK_SUCCESS;
    }

    uint64_t ticket = 0;
    if (recorded) {
        QueueSubmit submit;
        submit.commandBuffers.push_back(cmd);
        if (copyDone.semaphore != VK_NULL_HANDLE) submit.waits.push_back(copyDone);
        ticket = gfxQueue.submit(submit);
    }
    if (ticket == 0) {
        LOG_ERROR("SkiaGpu: an upload's copy or mip chain could not be recorded or submitted");
        if (gfxPool != VK_NULL_HANDLE) vkDestroyCommandPool(device, gfxPool, nullptr);
        if (staging != VK_NULL_HANDLE) vulkan_.destroyBuffer(staging, stagingAlloc);
        settle(GpuImageUpload::State::Failed);
        return;  // img retires with its last reference
    }

    up.vkImage_ = std::move(img);
    up.memory_ = memory;
    up.memoryOffset_ = offset;
    up.memorySize_ = req.size;
    up.levels_ = levels;
    up.ticket_.store(ticket, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(uploadMutex_);
        uploadStaging_.push_back({ticket, staging, stagingAlloc, gfxPool});
    }
    uploadCv_.notify_one();
    settle(GpuImageUpload::State::Submitted);
    // A canvas that held its replay back for this upload draws next frame.
    util::wakeMainLoop();
}

void SkiaGpu::freeUploadStaging(bool all) {
    if (all) vulkan_.queue().waitIdle();
    std::vector<UploadStaging> done;
    {
        std::lock_guard<std::mutex> lock(uploadMutex_);
        if (uploadStaging_.empty()) return;
        const uint64_t completed = vulkan_.queue().completedTicket();
        auto split = std::stable_partition(uploadStaging_.begin(), uploadStaging_.end(),
                                           [&](const UploadStaging& s) { return s.ticket > completed; });
        done.assign(split, uploadStaging_.end());
        uploadStaging_.erase(split, uploadStaging_.end());
    }
    for (const UploadStaging& s : done) {
        vkDestroyCommandPool(vulkan_.device(), s.pool, nullptr);
        if (s.buffer != VK_NULL_HANDLE) vulkan_.destroyBuffer(s.buffer, s.allocId);  // none: freed after the copy
    }
}

void SkiaGpu::stopUploads() {
    {
        std::lock_guard<std::mutex> lock(gTargetMutex);
        if (gTarget == this) gTarget = nullptr;
    }
    std::deque<std::weak_ptr<GpuImageUpload>> queued;
    std::unordered_map<uint64_t, PublishedUpload> published;
    {
        std::lock_guard<std::mutex> lock(uploadMutex_);
        uploadStop_ = true;
        queued.swap(uploadQueue_);
        published.swap(published_);
    }
    uploadCv_.notify_all();
    if (uploader_.joinable()) uploader_.join();
    // Uploads nobody started fail, so nothing waits on them forever.
    for (auto& weak : queued) {
        if (auto up = weak.lock()) {
            GpuImageUpload::State expected = GpuImageUpload::State::Queued;
            if (up->state_.compare_exchange_strong(expected, GpuImageUpload::State::Failed)) {
                std::lock_guard<std::mutex> lk(up->mu_);
                up->cv_.notify_all();
            }
        }
    }
    published.clear();
    while (uploadsRunning_.load(std::memory_order_acquire) > 0) std::this_thread::yield();

    // Uploads somebody still holds (a page's ImageBitmap outlives the GPU
    // context at teardown) give their texture back now, while the Skia
    // context their borrowed image unrefs into is alive; the SkiaImage then
    // retires with Skia's own last reference, at the context's release. The
    // upload becomes a Failed one with no GPU, which draws its raster image
    // and whose destructor reaches nothing.
    std::vector<std::weak_ptr<GpuImageUpload>> uploads;
    {
        std::lock_guard<std::mutex> lock(uploadMutex_);
        uploads.swap(uploads_);
    }
    for (auto& weak : uploads) {
        std::shared_ptr<GpuImageUpload> up = weak.lock();
        if (!up) continue;
        {
            Lock lock(*this);
            up->image_.reset();
            up->backend_.reset();
        }
        up->vkImage_.reset();
        up->ticket_.store(0, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lk(up->mu_);
            up->state_.store(GpuImageUpload::State::Failed, std::memory_order_release);
            up->gpu_ = nullptr;
        }
        up->cv_.notify_all();
    }
    freeUploadStaging(/*all=*/true);
}

}  // namespace bro::render
