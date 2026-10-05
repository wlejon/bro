#include "scene/vulkan/scene_gpu_timer.h"

#include "render/vulkan_frames.h"
#include "render/vulkan_queue.h"

#include <vector>

namespace bro::scene::vk {

SceneGpuTimer::~SceneGpuTimer() {
    if (pool_ == VK_NULL_HANDLE) return;
    VkQueryPool pool = pool_;
    VkDevice device = context_.device();
    context_.frames().defer([device, pool] { vkDestroyQueryPool(device, pool, nullptr); });
}

bool SceneGpuTimer::init() {
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(context_.physicalDevice(), &props);
    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(context_.physicalDevice(), &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(context_.physicalDevice(), &familyCount, families.data());
    const int family = context_.queueFamilies().graphicsFamily;
    if (family < 0 || static_cast<uint32_t>(family) >= familyCount || families[family].timestampValidBits == 0 ||
        props.limits.timestampPeriod <= 0.0f)
        return false;
    const uint32_t bits = families[family].timestampValidBits;
    validMask_ = bits >= 64 ? ~0ull : ((1ull << bits) - 1);
    periodNs_ = props.limits.timestampPeriod;

    VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = 2 * render::VulkanFrames::kFramesInFlight;
    return vkCreateQueryPool(context_.device(), &info, nullptr, &pool_) == VK_SUCCESS;
}

void SceneGpuTimer::begin(VkCommandBuffer cmd) {
    recordingPair_ = UINT32_MAX;
    if (pool_ == VK_NULL_HANDLE) return;
    const uint32_t pair = context_.frames().frameIndex();
    // A second render in the same frame slot would reset a pair the first may
    // still be writing; it goes untimed and the first keeps its result.
    if (pair == pendingPair_ && pendingTicket_ != 0 &&
        !context_.queue().isComplete(pendingTicket_))
        return;
    vkCmdResetQueryPool(cmd, pool_, 2 * pair, 2);
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, pool_, 2 * pair);
    recordingPair_ = pair;
}

void SceneGpuTimer::end(VkCommandBuffer cmd) {
    if (recordingPair_ == UINT32_MAX) return;
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pool_, 2 * recordingPair_ + 1);
}

void SceneGpuTimer::submitted(uint64_t ticket) {
    if (recordingPair_ == UINT32_MAX || ticket == 0) return;
    pendingPair_ = recordingPair_;
    pendingTicket_ = ticket;
    recordingPair_ = UINT32_MAX;
}

double SceneGpuTimer::takeMs() {
    if (pendingTicket_ == 0) return -1.0;
    const uint64_t ticket = pendingTicket_;
    pendingTicket_ = 0;
    if (!context_.queue().wait(ticket)) return -1.0;
    uint64_t stamps[2] = {};
    if (vkGetQueryPoolResults(context_.device(), pool_, 2 * pendingPair_, 2, sizeof(stamps),
                              stamps, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
        return -1.0;
    const uint64_t ticks = ((stamps[1] & validMask_) - (stamps[0] & validMask_)) & validMask_;
    return static_cast<double>(ticks) * periodNs_ / 1.0e6;
}

}  // namespace bro::scene::vk
