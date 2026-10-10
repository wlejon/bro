#include "render/skia_gpu.h"
#include "render/vulkan_util.h"
#include "util/log.h"

#include <include/core/SkCanvas.h>
#include <include/core/SkColorSpace.h>
#include <include/core/SkSurfaceProps.h>
#include <include/gpu/MutableTextureState.h>
#include <include/gpu/ganesh/GrBackendSurface.h>
#include <include/gpu/ganesh/GrContextOptions.h>
#include <include/gpu/ganesh/GrDirectContext.h>
#include <include/gpu/ganesh/SkSurfaceGanesh.h>
#include <include/gpu/ganesh/vk/GrVkBackendSurface.h>
#include <include/gpu/ganesh/vk/GrVkDirectContext.h>
#include <include/gpu/ganesh/vk/GrVkTypes.h>
#include <include/gpu/vk/VulkanBackendContext.h>
#include <include/gpu/vk/VulkanExtensions.h>
#include <include/gpu/vk/VulkanMutableTextureState.h>
#include <src/gpu/GpuTypesPriv.h>
#include <src/gpu/vk/vulkanmemoryallocator/VulkanMemoryAllocatorPriv.h>

#include <algorithm>
#include <cstring>

namespace bro::render {

// ---------------------------------------------------------------------------
// Skia's queue entry points, routed through the queue owner. Skia resolves
// every Vulkan function through the getProc it is given; these three replace
// the ones that touch the queue. They carry no user data, so the queues and
// devices they belong to are looked up in a small registry.
// ---------------------------------------------------------------------------

namespace {

struct Route {
    VkQueue queue;
    VkDevice device;
    VulkanQueue* owner;
};
std::mutex gRoutesMutex;
std::vector<Route> gRoutes;

VulkanQueue* ownerOfQueue(VkQueue queue) {
    std::lock_guard<std::mutex> lock(gRoutesMutex);
    for (const Route& r : gRoutes)
        if (r.queue == queue) return r.owner;
    return nullptr;
}

VulkanQueue* ownerOfDevice(VkDevice device) {
    std::lock_guard<std::mutex> lock(gRoutesMutex);
    for (const Route& r : gRoutes)
        if (r.device == device) return r.owner;
    return nullptr;
}

VKAPI_ATTR VkResult VKAPI_CALL routedQueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo* batches,
                                                 VkFence fence) {
    VulkanQueue* owner = ownerOfQueue(queue);
    if (!owner) {
        LOG_ERROR("SkiaGpu: submit on a queue no VulkanQueue owns");
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    return owner->submitForeign(count, batches, fence);
}

VKAPI_ATTR VkResult VKAPI_CALL routedQueueWaitIdle(VkQueue queue) {
    VulkanQueue* owner = ownerOfQueue(queue);
    return owner && owner->waitIdle() ? VK_SUCCESS : VK_ERROR_DEVICE_LOST;
}

// Skia's only device waits are at teardown; the queue is the only one it uses.
VKAPI_ATTR VkResult VKAPI_CALL routedDeviceWaitIdle(VkDevice device) {
    VulkanQueue* owner = ownerOfDevice(device);
    return owner && owner->waitIdle() ? VK_SUCCESS : VK_ERROR_DEVICE_LOST;
}

// Ganesh keeps a program's uniforms in push constants whenever they fit
// (maxPushConstantsSize, 256 bytes on most desktop drivers) and in a uniform
// buffer otherwise. Its gradient colorizer for three or more intervals (any
// CSS gradient with a stop not at 0% or 100%, or more than three stops) picks
// its interval by binary search and then reads `scale[pos]` / `bias[pos]` with
// a per-pixel index. RADV (Mesa 26.2, RDNA) gets a push-constant array read
// with a lane-divergent index wrong: whole waves take a neighbouring
// interval's scale and bias, so the ramp extrapolates past its stops in
// blocky, hue-shifted squares wherever a wave straddles a stop. The same read
// from a uniform buffer is correct, so Skia is told the device has no push
// constant space and puts every uniform in its uniform buffer: one path on
// every driver, at the cost of a descriptor bind per program change.
VKAPI_ATTR void VKAPI_CALL skiaPhysicalDeviceProperties(VkPhysicalDevice physicalDevice,
                                                        VkPhysicalDeviceProperties* properties) {
    vkGetPhysicalDeviceProperties(physicalDevice, properties);
    properties->limits.maxPushConstantsSize = 0;
}

PFN_vkVoidFunction skiaGetProc(const char* name, VkInstance instance, VkDevice device) {
    if (std::strcmp(name, "vkGetPhysicalDeviceProperties") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&skiaPhysicalDeviceProperties);
    if (std::strcmp(name, "vkQueueSubmit") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&routedQueueSubmit);
    if (std::strcmp(name, "vkQueueWaitIdle") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&routedQueueWaitIdle);
    if (std::strcmp(name, "vkDeviceWaitIdle") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&routedDeviceWaitIdle);
    if (device != VK_NULL_HANDLE) return vkGetDeviceProcAddr(device, name);
    return vkGetInstanceProcAddr(instance, name);
}

constexpr VkFormat kSurfaceFormat = VK_FORMAT_R8G8B8A8_UNORM;
// No INPUT_ATTACHMENT: with it Ganesh reads the destination of an advanced
// blend (multiply, screen, ...) as an input attachment in GENERAL layout, and
// its barrier out of GENERAL names HOST_WRITE under ALL_COMMANDS, which
// validation rejects (VUID-vkCmdPipelineBarrier-pImageMemoryBarriers-02819).
// Without it Ganesh copies the destination, as it did on GL.
constexpr VkImageUsageFlags kSurfaceUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

// Skia's reference to an image, dropped when Skia's own use of it is over.
void releaseSkiaRef(void* ctx) {
    delete static_cast<SkiaImageRef*>(ctx);
}

} // namespace

// ---------------------------------------------------------------------------

SkiaImage::~SkiaImage() {
    if (gpu_) gpu_->retire(image, view, allocId_);
}

void LayerSurface::reset() {
    if (image && surface) {
        // A GPU surface's last unref reaches the Skia context.
        SkiaGpu::Lock lock(*image->gpu_);
        surface.reset();
    }
    surface.reset();
    image.reset();
}

SkiaGpu::Lock::Lock(SkiaGpu& gpu) : gpu_(&gpu) {
    gpu.mutex_.lock();
    if (gpu.depth_++ == 0) gpu.owner_.store(std::this_thread::get_id(), std::memory_order_relaxed);
}

SkiaGpu::Lock::Lock(Lock&& other) noexcept : gpu_(other.gpu_) {
    other.gpu_ = nullptr;
}

SkiaGpu::Lock& SkiaGpu::Lock::operator=(Lock&& other) noexcept {
    if (this != &other) {
        this->~Lock();
        gpu_ = other.gpu_;
        other.gpu_ = nullptr;
    }
    return *this;
}

SkiaGpu::Lock::~Lock() {
    if (!gpu_) return;
    if (--gpu_->depth_ == 0) gpu_->owner_.store(std::thread::id{}, std::memory_order_relaxed);
    gpu_->mutex_.unlock();
    gpu_ = nullptr;
}

SkiaGpu::SkiaGpu(VulkanContext& vulkan) : vulkan_(vulkan) {}

SkiaGpu::~SkiaGpu() {
    // The uploader thread, the uploads it still held, and their staging
    // buffers, while the context they borrow textures from is alive.
    stopUploads();
    if (context_) {
        Lock lock(*this);
        // Frees everything Skia allocated on the device and invokes the
        // release procs of the images it still referenced. A surface somebody
        // still holds stays a valid (abandoned) object.
        context_->flushAndSubmit(GrSyncCpu::kYes);
        context_->storeVkPipelineCacheData();
        if (persistentCache_) persistentCache_->save();
        context_->releaseResourcesAndAbandonContext();
        context_.reset();
    }
    {
        std::lock_guard<std::mutex> lock(gRoutesMutex);
        std::erase_if(gRoutes, [&](const Route& r) { return r.owner == &vulkan_.queue(); });
    }
    vulkan_.queue().waitIdle();
    collect();
    if (const int live = liveImages_.load(); live > 0)
        LOG_ERROR("SkiaGpu: %d surface image(s) outlived the Skia context", live);
}

bool SkiaGpu::init() {
    if (!vulkan_.isValid()) return false;
    {
        std::lock_guard<std::mutex> lock(gRoutesMutex);
        gRoutes.push_back({vulkan_.graphicsQueue(), vulkan_.device(), &vulkan_.queue()});
    }

    std::vector<const char*> instanceExts, deviceExts;
    for (const auto& e : vulkan_.enabledInstanceExtensions()) instanceExts.push_back(e.c_str());
    for (const auto& e : vulkan_.enabledDeviceExtensions()) deviceExts.push_back(e.c_str());
    skgpu::VulkanExtensions extensions;
    extensions.init(skiaGetProc, vulkan_.instance(), vulkan_.physicalDevice(),
                    static_cast<uint32_t>(instanceExts.size()), instanceExts.data(),
                    static_cast<uint32_t>(deviceExts.size()), deviceExts.data());

    skgpu::VulkanBackendContext backend;
    backend.fInstance = vulkan_.instance();
    backend.fPhysicalDevice = vulkan_.physicalDevice();
    backend.fDevice = vulkan_.device();
    backend.fQueue = vulkan_.graphicsQueue();
    backend.fGraphicsQueueIndex = static_cast<uint32_t>(vulkan_.queueFamilies().graphicsFamily);
    backend.fMaxAPIVersion = vulkan_.apiVersion();
    backend.fVkExtensions = &extensions;
    backend.fDeviceFeatures = &vulkan_.features();
    backend.fGetProc = skiaGetProc;
    // Skia's own VMA-based allocator (its GN build does not make one by
    // itself). Not internally locked: every Skia call runs under lock().
    backend.fMemoryAllocator = skgpu::VulkanMemoryAllocators::Make(backend, skgpu::ThreadSafe::kNo);
    memoryAllocator_ = backend.fMemoryAllocator;
    if (!backend.fMemoryAllocator) {
        LOG_ERROR("SkiaGpu: Skia could not create its Vulkan memory allocator; drawing on the CPU");
        std::lock_guard<std::mutex> lock(gRoutesMutex);
        std::erase_if(gRoutes, [&](const Route& r) { return r.owner == &vulkan_.queue(); });
        return false;
    }

    // Ganesh's shaders and pipeline cache persist beside the device's own.
    std::string cachePath = vulkan_.persistentPipelineCache().path();
    if (cachePath.size() > 4 && cachePath.ends_with(".bin")) cachePath.insert(cachePath.size() - 4, ".skia");
    persistentCache_ = std::make_unique<SkiaPersistentCache>(cachePath);

    GrContextOptions options;
    options.fPersistentCache = persistentCache_.get();
    context_ = GrDirectContexts::MakeVulkan(backend, options);
    if (!context_) {
        LOG_ERROR("SkiaGpu: Skia could not create a Vulkan context on this device; drawing on the CPU");
        std::lock_guard<std::mutex> lock(gRoutesMutex);
        std::erase_if(gRoutes, [&](const Route& r) { return r.owner == &vulkan_.queue(); });
        return false;
    }
    LOG_INFO("SkiaGpu: Ganesh on Vulkan");
    becomeUploadTarget(this);
    return true;
}

LayerSurface SkiaGpu::makeSurface(int width, int height) {
    if (!context_ || width <= 0 || height <= 0) return {};

    auto img = std::make_shared<SkiaImage>();
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    if (!vulkan_.createImage(static_cast<uint32_t>(width), static_cast<uint32_t>(height), kSurfaceFormat,
                             VK_IMAGE_TILING_OPTIMAL, kSurfaceUsage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, img->image,
                             memory, offset, img->allocId_)) {
        LOG_ERROR("SkiaGpu: failed to create a %dx%d surface image", width, height);
        return {};
    }
    img->gpu_ = this;  // from here on the destructor releases it
    liveImages_.fetch_add(1, std::memory_order_relaxed);
    img->format = kSurfaceFormat;
    img->width = static_cast<uint32_t>(width);
    img->height = static_cast<uint32_t>(height);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = img->image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = kSurfaceFormat;
    viewInfo.subresourceRange = colorRange();
    if (vkCreateImageView(vulkan_.device(), &viewInfo, nullptr, &img->view) != VK_SUCCESS) {
        LOG_ERROR("SkiaGpu: failed to create a surface image view");
        return {};
    }

    GrVkImageInfo info;
    info.fImage = img->image;
    info.fAlloc.fMemory = memory;
    info.fAlloc.fOffset = offset;
    info.fImageTiling = VK_IMAGE_TILING_OPTIMAL;
    info.fImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    info.fFormat = kSurfaceFormat;
    info.fImageUsageFlags = kSurfaceUsage;
    info.fSampleCount = 1;
    info.fLevelCount = 1;
    const GrBackendTexture texture = GrBackendTextures::MakeVk(width, height, info);

    Lock lock(*this);
    const SkSurfaceProps props(0, kUnknown_SkPixelGeometry);
    // Skia holds its own reference until its last use of the image is over.
    sk_sp<SkSurface> surface = SkSurfaces::WrapBackendTexture(
        context_.get(), texture, kTopLeft_GrSurfaceOrigin, 1, kRGBA_8888_SkColorType, nullptr, &props,
        releaseSkiaRef, new SkiaImageRef(img));
    if (!surface) {
        LOG_ERROR("SkiaGpu: Skia could not wrap a %dx%d surface image", width, height);
        return {};
    }
    surface->getCanvas()->clear(SK_ColorTRANSPARENT);
    finish(surface.get());
    return LayerSurface(std::move(surface), std::move(img));
}

void SkiaGpu::finish(std::span<SkSurface* const> surfaces) {
    if (!context_) return;
    Lock lock(*this);
    const skgpu::MutableTextureState readable =
        skgpu::MutableTextureStates::MakeVulkan(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_QUEUE_FAMILY_IGNORED);
    if (surfaces.empty()) context_->flush(GrFlushInfo{});
    for (SkSurface* s : surfaces)
        if (s) context_->flush(s, GrFlushInfo{}, &readable);
    context_->submit(GrSyncCpu::kNo);
}

void SkiaGpu::retire(VkImage image, VkImageView view, uint64_t allocId) {
    // Every submission that could still use the image has been made: Skia's
    // (its release proc ran) and whoever composited it (they dropped their
    // reference after submitting).
    const uint64_t ticket = vulkan_.queue().retireTicket();
    std::lock_guard<std::mutex> lock(retireMutex_);
    retired_.push_back({image, view, allocId, ticket});
    liveImages_.fetch_sub(1, std::memory_order_relaxed);
}

void SkiaGpu::collect() {
    freeUploadStaging(/*all=*/false);
    std::vector<Retired> done;
    {
        std::lock_guard<std::mutex> lock(retireMutex_);
        if (retired_.empty()) return;
        const uint64_t completed = vulkan_.queue().completedTicket();
        auto split = std::partition(retired_.begin(), retired_.end(),
                                    [&](const Retired& r) { return r.ticket > completed; });
        done.assign(split, retired_.end());
        retired_.erase(split, retired_.end());
    }
    for (const Retired& r : done) {
        if (r.view != VK_NULL_HANDLE) vkDestroyImageView(vulkan_.device(), r.view, nullptr);
        vulkan_.destroyImage(r.image, r.allocId);
    }
}

} // namespace bro::render
