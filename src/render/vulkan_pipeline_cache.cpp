#include "render/vulkan_pipeline_cache.h"
#include "util/log.h"
#include "util/user_dirs.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <process.h>
#define BRO_GETPID _getpid
#else
#include <unistd.h>
#define BRO_GETPID getpid
#endif

namespace bro::render {

namespace {

// A cache past this size is not written back: the next launch starts empty
// and rebuilds only what it uses, which bounds the file without an LRU.
constexpr size_t kMaxPersistedBytes = 128u << 20;

std::mutex s_registryMutex;
std::vector<std::pair<VkDevice, VkPipelineCache>> s_registry;

std::string cacheDirectory() {
    if (const char* off = std::getenv("BRO_PIPELINE_CACHE"); off && std::strcmp(off, "0") == 0)
        return {};
    if (const char* dir = std::getenv("BRO_PIPELINE_CACHE_DIR"); dir && *dir) return dir;
    return util::userCacheDir() + "/pipeline-cache";
}

std::string cacheFileName(const VkPhysicalDeviceProperties& props) {
    char head[64];
    std::snprintf(head, sizeof(head), "%04x-%04x-%08x-", props.vendorID, props.deviceID,
                  props.driverVersion);
    std::string name = head;
    for (uint8_t b : props.pipelineCacheUUID) {
        char hex[3];
        std::snprintf(hex, sizeof(hex), "%02x", b);
        name += hex;
    }
    return name + ".bin";
}

// VkPipelineCacheHeaderVersionOne, checked field by field: a blob from another
// device or driver, a truncated file, or garbage is dropped here rather than
// trusted to the driver's own validation.
bool headerMatches(const std::vector<char>& data, const VkPhysicalDeviceProperties& props) {
    if (data.size() < 32) return false;
    uint32_t words[4];
    std::memcpy(words, data.data(), sizeof(words));
    return words[0] >= 32 && words[0] <= data.size() &&
           words[1] == VK_PIPELINE_CACHE_HEADER_VERSION_ONE &&
           words[2] == props.vendorID && words[3] == props.deviceID &&
           std::memcmp(data.data() + 16, props.pipelineCacheUUID, VK_UUID_SIZE) == 0;
}

} // namespace

void VulkanPipelineCache::init(VkDevice device, const VkPhysicalDeviceProperties& props) {
    device_ = device;
    const std::string dir = cacheDirectory();
    if (!dir.empty()) path_ = dir + "/" + cacheFileName(props);

    std::vector<char> seed;
    if (!path_.empty()) {
        std::ifstream in(path_, std::ios::binary);
        if (in) {
            seed.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            if (!headerMatches(seed, props)) {
                LOG_WARN("Vulkan: ignoring pipeline cache %s (not this device/driver's)",
                         path_.c_str());
                seed.clear();
            }
        }
    }

    VkPipelineCacheCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    info.initialDataSize = seed.size();
    info.pInitialData = seed.empty() ? nullptr : seed.data();
    VkResult res = vkCreatePipelineCache(device_, &info, nullptr, &cache_);
    if (res != VK_SUCCESS && !seed.empty()) {
        LOG_WARN("Vulkan: driver rejected the pipeline cache %s (%d); starting empty",
                 path_.c_str(), res);
        seed.clear();
        info.initialDataSize = 0;
        info.pInitialData = nullptr;
        res = vkCreatePipelineCache(device_, &info, nullptr, &cache_);
    }
    if (res != VK_SUCCESS) {
        LOG_ERROR("Vulkan: vkCreatePipelineCache failed (%d); pipelines build uncached", res);
        cache_ = VK_NULL_HANDLE;
        return;
    }
    loadedSize_ = seed.size();
    LOG_INFO("Vulkan: pipeline cache %s (%zu bytes loaded)",
             path_.empty() ? "in memory only" : path_.c_str(), loadedSize_);

    std::lock_guard<std::mutex> lock(s_registryMutex);
    s_registry.emplace_back(device_, cache_);
}

void VulkanPipelineCache::shutdown() {
    if (cache_ == VK_NULL_HANDLE) return;
    {
        std::lock_guard<std::mutex> lock(s_registryMutex);
        s_registry.erase(std::remove_if(s_registry.begin(), s_registry.end(),
                                        [&](const auto& e) { return e.first == device_; }),
                         s_registry.end());
    }

    size_t size = 0;
    if (!path_.empty() && vkGetPipelineCacheData(device_, cache_, &size, nullptr) == VK_SUCCESS &&
        size > loadedSize_ && size <= kMaxPersistedBytes) {
        std::vector<char> data(size);
        if (vkGetPipelineCacheData(device_, cache_, &size, data.data()) == VK_SUCCESS &&
            !writeCacheFile(path_, data.data(), size))
            LOG_WARN("Vulkan: could not write the pipeline cache to %s", path_.c_str());
    }

    vkDestroyPipelineCache(device_, cache_, nullptr);
    cache_ = VK_NULL_HANDLE;
}

bool writeCacheFile(const std::string& path, const void* data, size_t size) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    const std::string tmp = path + ".tmp." + std::to_string(BRO_GETPID());
    bool written = false;
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
        written = static_cast<bool>(out);
    }
    if (written) fs::rename(tmp, path, ec);
    if (!written || ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

VkPipelineCache VulkanPipelineCache::forDevice(VkDevice device) {
    std::lock_guard<std::mutex> lock(s_registryMutex);
    for (const auto& [dev, cache] : s_registry)
        if (dev == device) return cache;
    return VK_NULL_HANDLE;
}

} // namespace bro::render
