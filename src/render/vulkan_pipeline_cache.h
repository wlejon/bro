#pragma once

#include <vulkan/vulkan.h>
#include <cstddef>
#include <string>

namespace bro::render {

/// The device's VkPipelineCache, persisted across runs so a warm launch skips
/// the driver's shader compiles for every pipeline it has built before (the
/// scene's passes, WebGL programs, the presenter).
///
/// It lives in `<user cache dir>/pipeline-cache/` — beside bronze's code cache
/// (docs/code-cache.md) — one file per device + driver, named by the cache
/// UUID the driver reports, so two GPUs or a driver update never read each
/// other's data. The file is checked against the header the driver expects
/// (VkPipelineCacheHeaderVersionOne: vendor, device, UUID) before it is handed
/// over, since not every driver survives a foreign blob. It is written back at
/// device teardown when the cache grew, to a temporary file renamed into place,
/// so concurrent processes and crashes mid-write never leave a torn file.
///
/// BRO_PIPELINE_CACHE=0 keeps the cache in memory only; BRO_PIPELINE_CACHE_DIR
/// moves it. Deleting the directory is always safe.
class VulkanPipelineCache {
public:
    VulkanPipelineCache() = default;
    ~VulkanPipelineCache() = default;
    VulkanPipelineCache(const VulkanPipelineCache&) = delete;
    VulkanPipelineCache& operator=(const VulkanPipelineCache&) = delete;

    /// Create the cache for `device`, seeded from disk when a matching file
    /// exists. A cache that cannot be created at all leaves handle() null,
    /// which every vkCreate*Pipelines accepts.
    void init(VkDevice device, const VkPhysicalDeviceProperties& props);
    /// Write the cache back (if it grew) and destroy it. Before vkDestroyDevice.
    void shutdown();

    VkPipelineCache handle() const { return cache_; }

    /// The cache of the VulkanContext that owns `device`, for code that builds
    /// pipelines from a bare VkDevice (the scene's pipeline builder). Null for
    /// a device no context registered.
    static VkPipelineCache forDevice(VkDevice device);

    /// The file this device's cache persists to ("" when persistence is off).
    const std::string& path() const { return path_; }
    /// Bytes seeded from that file at init (0 for a cold start).
    size_t loadedBytes() const { return loadedSize_; }

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkPipelineCache cache_ = VK_NULL_HANDLE;
    std::string path_;
    size_t loadedSize_ = 0;
};

/// Write `size` bytes to `path` through a per-process temp file renamed over
/// it, creating the directory: concurrent writers and a crash mid-write never
/// leave a torn file. False when it could not be written.
bool writeCacheFile(const std::string& path, const void* data, size_t size);

} // namespace bro::render
