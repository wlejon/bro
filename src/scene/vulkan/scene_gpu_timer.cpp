#include "scene/vulkan/scene_gpu_timer.h"

#include "render/vulkan_frames.h"
#include "render/vulkan_queue.h"
#include "util/log.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace bro::scene::vk {

static_assert(render::VulkanFrames::kFramesInFlight <= 8, "SceneGpuTimer keeps marks for at most 8 slots");

SceneGpuTimer::~SceneGpuTimer() {
    VkDevice device = context_.device();
    for (VkQueryPool pool : {pool_, statPool_})
        if (pool != VK_NULL_HANDLE) context_.frames().defer([device, pool] { vkDestroyQueryPool(device, pool, nullptr); });
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
    if (const char* env = std::getenv("BRO_SCENE_GPU_PASSES"); env && *env) passLogMs_ = std::atof(env);

    VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = kPerSlot * render::VulkanFrames::kFramesInFlight;
    // BRO_SCENE_GPU_FRAGS: each opaque draw's fragment shader invocations too
    // (a pipeline-statistics query a draw): overdraw, which the times alone
    // don't tell from a costly shader.
    if (const char* env = std::getenv("BRO_SCENE_GPU_FRAGS");
        env && *env && passLogMs_ >= 0.0 && context_.features().pipelineStatisticsQuery) {
        VkQueryPoolCreateInfo s{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        s.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
        s.pipelineStatistics = VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT;
        s.queryCount = kMaxStats * render::VulkanFrames::kFramesInFlight;
        vkCreateQueryPool(context_.device(), &s, nullptr, &statPool_);
    }
    return vkCreateQueryPool(context_.device(), &info, nullptr, &pool_) == VK_SUCCESS;
}

void SceneGpuTimer::begin(VkCommandBuffer cmd) {
    recordingPair_ = UINT32_MAX;
    if (pool_ == VK_NULL_HANDLE) return;
    const uint32_t pair = context_.frames().frameIndex();
    // A second render in the same frame slot would reset queries the first
    // may still be writing; it goes untimed and the first keeps its result.
    if (pair == pendingPair_ && pendingTicket_ != 0 &&
        !context_.queue().isComplete(pendingTicket_))
        return;
    vkCmdResetQueryPool(cmd, pool_, kPerSlot * pair, kPerSlot);
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, pool_, kPerSlot * pair);
    recordingPair_ = pair;
    markCount_[pair] = 0;
    statCount_[pair] = 0;
    if (statPool_ != VK_NULL_HANDLE) vkCmdResetQueryPool(cmd, statPool_, kMaxStats * pair, kMaxStats);
}

void SceneGpuTimer::statBegin(VkCommandBuffer cmd, const char* name) {
    if (statPool_ == VK_NULL_HANDLE || recordingPair_ == UINT32_MAX) return;
    uint32_t& n = statCount_[recordingPair_];
    if (n >= kMaxStats) return;
    vkCmdBeginQuery(cmd, statPool_, kMaxStats * recordingPair_ + n, 0);
    statNames_[recordingPair_][n++] = name;
    statOpen_ = true;
}

void SceneGpuTimer::statEnd(VkCommandBuffer cmd) {
    if (!statOpen_) return;
    statOpen_ = false;
    vkCmdEndQuery(cmd, statPool_, kMaxStats * recordingPair_ + statCount_[recordingPair_] - 1);
}

void SceneGpuTimer::mark(VkCommandBuffer cmd, const char* name) {
    if (passLogMs_ < 0.0 || recordingPair_ == UINT32_MAX) return;
    uint32_t& n = markCount_[recordingPair_];
    if (n >= kMaxMarks) return;
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pool_, kPerSlot * recordingPair_ + 2 + n);
    markNames_[recordingPair_][n++] = name;
}

void SceneGpuTimer::end(VkCommandBuffer cmd) {
    if (recordingPair_ == UINT32_MAX) return;
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pool_, kPerSlot * recordingPair_ + 1);
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
    const uint32_t marks = markCount_[pendingPair_];
    uint64_t stamps[kPerSlot] = {};
    if (vkGetQueryPoolResults(context_.device(), pool_, kPerSlot * pendingPair_, 2 + marks, sizeof(stamps),
                              stamps, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
        return -1.0;
    const auto ms = [&](uint64_t from, uint64_t to) {
        const uint64_t ticks = ((to & validMask_) - (from & validMask_)) & validMask_;
        return static_cast<double>(ticks) * periodNs_ / 1.0e6;
    };
    const double total = ms(stamps[0], stamps[1]);
    if (passLogMs_ >= 0.0 && marks > 0 && total > passLogMs_) {
        std::string line;
        uint64_t prev = stamps[0];
        char buf[96];
        for (uint32_t i = 0; i < marks; ++i) {
            const double d = ms(prev, stamps[2 + i]);
            prev = stamps[2 + i];
            // (A shadow tile's end stays, short or not: it bounds the tile's marks.)
            if (d < 0.01 && std::strncmp(markNames_[pendingPair_][i], "shadow:tile", 11) != 0) continue;
            std::snprintf(buf, sizeof(buf), " %s %.2f", markNames_[pendingPair_][i], d);
            line += buf;
        }
        std::snprintf(buf, sizeof(buf), " rest %.2f", ms(prev, stamps[1]));
        line += buf;
        // (Past kMaxMarks the render's later marks were not written: their
        // time is in `rest`.)
        if (marks >= kMaxMarks) line += " (marks full)";
        LOG_INFO("scene GPU %.2f ms:%s", total, line.c_str());
        if (const uint32_t ns = statCount_[pendingPair_]; statPool_ != VK_NULL_HANDLE && ns > 0) {
            uint64_t inv[kMaxStats] = {};
            std::string s;
            if (vkGetQueryPoolResults(context_.device(), statPool_, kMaxStats * pendingPair_, ns, sizeof(inv), inv,
                                      sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS) {
                for (uint32_t i = 0; i < ns; ++i) {
                    std::snprintf(buf, sizeof(buf), " %s %llu", statNames_[pendingPair_][i],
                                  static_cast<unsigned long long>(inv[i]));
                    s += buf;
                }
                LOG_INFO("scene FRAG:%s", s.c_str());
            }
        }
    }
    return total;
}

}  // namespace bro::scene::vk
