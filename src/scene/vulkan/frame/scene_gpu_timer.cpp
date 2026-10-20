#include "scene/vulkan/frame/scene_gpu_timer.h"

#include "render/vulkan_frames.h"
#include "render/vulkan_queue.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <iterator>
#include <vector>

namespace bro::scene::vk {

static_assert(render::VulkanFrames::kFramesInFlight <= 8, "SceneGpuTimer keeps marks for at most 8 slots");

namespace {
std::atomic<bool> g_timePasses{false};

// `name`'s entry in `passes` grown by `ms` (a name marked twice, summed).
void addPass(SceneGpuTimer::Passes& passes, const char* name, double ms) {
    for (auto& [n, v] : passes)
        if (n == name) {
            v += ms;
            return;
        }
    passes.emplace_back(name, ms);
}
}  // namespace

void SceneGpuTimer::timePasses() { g_timePasses.store(true, std::memory_order_relaxed); }

SceneGpuTimer::~SceneGpuTimer() {
    VkDevice device = context_.device();
    for (VkQueryPool pool : {pool_, computePool_})
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

    VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = kPerSlot * render::VulkanFrames::kFramesInFlight;
    // The async compute queue's stamps, when its family writes them.
    const int computeFamily = context_.queueFamilies().computeFamily;
    if (context_.asyncComputeQueue() && computeFamily >= 0 && static_cast<uint32_t>(computeFamily) < familyCount &&
        families[computeFamily].timestampValidBits > 0) {
        const uint32_t cbits = families[computeFamily].timestampValidBits;
        computeMask_ = cbits >= 64 ? ~0ull : ((1ull << cbits) - 1);
        if (vkCreateQueryPool(context_.device(), &info, nullptr, &computePool_) != VK_SUCCESS)
            computePool_ = VK_NULL_HANDLE;
    }
    return vkCreateQueryPool(context_.device(), &info, nullptr, &pool_) == VK_SUCCESS;
}

void SceneGpuTimer::beginCompute(VkCommandBuffer cmd) {
    computeCmd_ = VK_NULL_HANDLE;
    if (computePool_ == VK_NULL_HANDLE || recordingPair_ == UINT32_MAX) return;
    const uint32_t pair = recordingPair_;
    vkCmdResetQueryPool(cmd, computePool_, kPerSlot * pair, kPerSlot);
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, computePool_, kPerSlot * pair);
    computeCmd_ = cmd;
    computeTimed_[pair] = true;
    computeMarkCount_[pair] = 0;
}

void SceneGpuTimer::endCompute(VkCommandBuffer cmd) {
    if (cmd == VK_NULL_HANDLE || cmd != computeCmd_ || recordingPair_ == UINT32_MAX) return;
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, computePool_, kPerSlot * recordingPair_ + 1);
}

void SceneGpuTimer::begin(VkCommandBuffer cmd) {
    recordingPair_ = UINT32_MAX;
    if (pool_ == VK_NULL_HANDLE) return;
    const uint32_t pair = context_.frames().frameIndex();
    // A second render in the same frame slot would reset queries the first
    // may still be writing; it goes untimed and the first keeps its result.
    // A finished one not taken yet is read before its queries are reset.
    if (pendingTicket_[pair] != 0) {
        if (!context_.queue().isComplete(pendingTicket_[pair])) return;
        const double ms = harvest(pair);
        if (ms >= 0.0) {
            if (readyCount_ == std::size(ready_)) {
                std::copy(ready_ + 1, ready_ + readyCount_, ready_);
                --readyCount_;
            }
            ready_[readyCount_++] = ms;
        }
    }
    vkCmdResetQueryPool(cmd, pool_, kPerSlot * pair, kPerSlot);
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, pool_, kPerSlot * pair);
    recordingPair_ = pair;
    markCount_[pair] = 0;
    computeTimed_[pair] = false;
    computeCmd_ = VK_NULL_HANDLE;
}

void SceneGpuTimer::mark(VkCommandBuffer cmd, const char* name) {
    if (recordingPair_ == UINT32_MAX || !g_timePasses.load(std::memory_order_relaxed)) return;
    if (cmd == computeCmd_ && cmd != VK_NULL_HANDLE) {
        // (The compute stream's own stage: its queue's clock.)
        uint32_t& c = computeMarkCount_[recordingPair_];
        if (c >= kMaxMarks) return;
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, computePool_,
                            kPerSlot * recordingPair_ + 2 + c);
        computeMarkNames_[recordingPair_][c++] = name;
        return;
    }
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
    pendingTicket_[recordingPair_] = ticket;
    pendingOrder_[recordingPair_] = ++submits_;
    recordingPair_ = UINT32_MAX;
    computeCmd_ = VK_NULL_HANDLE;
}

void SceneGpuTimer::drain(bool wait) {
    for (;;) {
        uint32_t pair = UINT32_MAX;
        for (uint32_t i = 0; i < render::VulkanFrames::kFramesInFlight; ++i)
            if (pendingTicket_[i] != 0 && (pair == UINT32_MAX || pendingOrder_[i] < pendingOrder_[pair])) pair = i;
        if (pair == UINT32_MAX) break;
        if (!context_.queue().isComplete(pendingTicket_[pair])) {
            if (!wait) break;
            if (!context_.queue().wait(pendingTicket_[pair])) {
                pendingTicket_[pair] = 0;
                continue;
            }
        }
        const double ms = harvest(pair);
        if (ms < 0.0) continue;
        if (readyCount_ == std::size(ready_)) {
            std::copy(ready_ + 1, ready_ + readyCount_, ready_);
            --readyCount_;
        }
        ready_[readyCount_++] = ms;
    }
}

double SceneGpuTimer::takeMs(bool wait) {
    drain(wait);
    if (readyCount_ == 0) return -1.0;
    if (wait) {
        const double last = ready_[readyCount_ - 1];
        readyCount_ = 0;
        return last;
    }
    const double first = ready_[0];
    std::copy(ready_ + 1, ready_ + readyCount_, ready_);
    --readyCount_;
    return first;
}

bool SceneGpuTimer::lastPasses(Passes& passes, Passes& compute, bool& computed) {
    drain(true);
    if (!havePasses_) return false;
    passes = lastPasses_;
    compute = lastCompute_;
    computed = lastComputed_;
    return true;
}

double SceneGpuTimer::harvest(uint32_t pair) {
    pendingTicket_[pair] = 0;
    const uint32_t marks = markCount_[pair];
    uint64_t stamps[kPerSlot] = {};
    if (vkGetQueryPoolResults(context_.device(), pool_, kPerSlot * pair, 2 + marks, sizeof(stamps),
                              stamps, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
        return -1.0;
    const auto ms = [&](uint64_t from, uint64_t to) {
        const uint64_t ticks = ((to & validMask_) - (from & validMask_)) & validMask_;
        return static_cast<double>(ticks) * periodNs_ / 1.0e6;
    };
    // The compute stream's stamps, on the same device clock.
    const uint32_t cmarks = computeTimed_[pair] ? computeMarkCount_[pair] : 0;
    uint64_t cstamps[kPerSlot] = {};
    const bool computed = computeTimed_[pair] &&
                          vkGetQueryPoolResults(context_.device(), computePool_, kPerSlot * pair, 2 + cmarks,
                                                sizeof(cstamps), cstamps, sizeof(uint64_t),
                                                VK_QUERY_RESULT_64_BIT) == VK_SUCCESS;
    computeTimed_[pair] = false;
    uint64_t start = stamps[0] & validMask_, end = stamps[1] & validMask_;
    if (computed) {
        start = std::min(start, cstamps[0] & computeMask_);
        end = std::max(end, cstamps[1] & computeMask_);
    }
    // Wall time from the later of its start and the last render's end.
    const uint64_t from = lastEnd_ > start && lastEnd_ < end ? lastEnd_ : start;
    lastEnd_ = end;
    const double total = static_cast<double>(end - from) * periodNs_ / 1.0e6;
    if (marks > 0 || cmarks > 0) {
        // Its passes, each from the mark before it. (Past kMaxMarks the
        // render's later marks were not written: their time is in `rest`.)
        lastPasses_.clear();
        uint64_t prev = stamps[0];
        for (uint32_t i = 0; i < marks; ++i) {
            addPass(lastPasses_, markNames_[pair][i], ms(prev, stamps[2 + i]));
            prev = stamps[2 + i];
        }
        addPass(lastPasses_, "rest", ms(prev, stamps[1]));
        // The compute queue's stages, from its own start.
        lastCompute_.clear();
        lastComputed_ = computed;
        if (computed) {
            const auto cms = [&](uint64_t a, uint64_t b) {
                return static_cast<double>(((b & computeMask_) - (a & computeMask_)) & computeMask_) * periodNs_ /
                       1.0e6;
            };
            uint64_t at = cstamps[0];
            for (uint32_t i = 0; i < cmarks; ++i) {
                addPass(lastCompute_, computeMarkNames_[pair][i], cms(at, cstamps[2 + i]));
                at = cstamps[2 + i];
            }
            addPass(lastCompute_, "rest", cms(at, cstamps[1]));
        }
        havePasses_ = true;
    }
    return total;
}

}  // namespace bro::scene::vk
