#pragma once

// What a pipeline must agree with about the attachments it draws into:
// formats, count and sample count. A pass that can draw into more than one
// target (the HDR scope at 1x or MSAA, with or without the indirect-light
// attachment; a probe face at 1x) keeps one pipeline per TargetFormat in a
// PipelineVariants map instead of rebuilding when the target changes.

#include "scene/vulkan/frame/scene_vk_device.h"

#include <vulkan/vulkan.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <unordered_map>
#include <vector>

namespace bro::scene::vk {

/// Graphics pipelines built off the frame thread. While a Capture is
/// current on a thread, the first SceneVkPipelineBuilder::build there hands
/// its create-info to a thread of its own and returns null; `future` is the
/// pipeline. (scene_vk_pipeline.cpp)
namespace pipeline_async {
struct Capture {
    std::shared_future<VkPipeline> future;
    bool used = false;
};
Capture*& current();
/// Pipelines being built off the frame thread now (pendingWork).
int inFlight();
}  // namespace pipeline_async

struct TargetFormat {
    static constexpr uint32_t kMaxColor = 4;   // (a surface layer tile's fill writes four)

    VkFormat color[kMaxColor] = {VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED};
    uint32_t colorCount = 0;
    VkFormat depth = VK_FORMAT_UNDEFINED;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;

    static TargetFormat colorOnly(VkFormat c, VkSampleCountFlagBits s = VK_SAMPLE_COUNT_1_BIT) {
        TargetFormat t;
        t.color[0] = c;
        t.colorCount = 1;
        t.samples = s;
        return t;
    }

    /// A compact identity: core VkFormat values fit in 12 bits.
    uint64_t key() const {
        return (static_cast<uint64_t>(color[0]) & 0xFFF) |
               ((static_cast<uint64_t>(color[1]) & 0xFFF) << 12) |
               ((static_cast<uint64_t>(depth) & 0xFFF) << 24) |
               (static_cast<uint64_t>(samples) << 36) |
               (static_cast<uint64_t>(colorCount) << 44);
    }
    /// The third and fourth colour formats (key() holds the rest).
    uint32_t keyMore() const {
        return static_cast<uint32_t>((static_cast<uint64_t>(color[2]) & 0xFFF) |
                                     ((static_cast<uint64_t>(color[3]) & 0xFFF) << 12));
    }
    bool operator==(const TargetFormat& o) const { return key() == o.key() && keyMore() == o.keyMore(); }
    bool operator!=(const TargetFormat& o) const { return !(*this == o); }
};

/// Pipelines of one pass keyed by (variant, TargetFormat), built on first use
/// and destroyed with the pass. `variant` is the pass's own small index
/// (blend mode, vertex layout, ...), below 2^16.
///
/// With `async`, a pipeline not yet built is built off the frame thread
/// (pipeline_async): null until it is in, so its draw is skipped, never
/// waited for.
class PipelineVariants {
public:
    VkPipeline get(uint32_t variant, const TargetFormat& target,
                   const std::function<VkPipeline()>& build, bool async = false) {
        const Key key{target.key() | (static_cast<uint64_t>(variant) << 48), target.keyMore()};
        auto it = pipelines_.find(key);
        if (it != pipelines_.end()) return it->second;
        if (auto pit = pending_.find(key); pit != pending_.end()) {
            if (pit->second.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return VK_NULL_HANDLE;
            VkPipeline p = pit->second.get();
            pending_.erase(pit);
            if (p != VK_NULL_HANDLE) pipelines_.emplace(key, p);
            return p;
        }
        pipeline_async::Capture capture;
        pipeline_async::Capture*& cur = pipeline_async::current();
        pipeline_async::Capture* prev = cur;
        if (async) cur = &capture;
        VkPipeline p = build();
        cur = prev;
        if (capture.used) {
            pending_.emplace(key, capture.future);
            return VK_NULL_HANDLE;
        }
        if (p != VK_NULL_HANDLE) pipelines_.emplace(key, p);
        return p;
    }

    /// Destroy every pipeline once the GPU is done with frames using them
    /// (those still being built once they are).
    void destroy(SceneVkDevice& device) {
        for (auto& [k, f] : pending_)
            if (VkPipeline p = f.get()) pipelines_.emplace(k, p);
        pending_.clear();
        if (pipelines_.empty()) return;
        VkDevice dev = device.device();
        device.defer([dev, ps = std::move(pipelines_)] {
            for (auto& [k, p] : ps) vkDestroyPipeline(dev, p, nullptr);
        });
        pipelines_.clear();
    }

private:
    struct Key {
        uint64_t lo;
        uint32_t more;
        bool operator==(const Key& o) const { return lo == o.lo && more == o.more; }
    };
    struct KeyHash {
        size_t operator()(const Key& k) const {
            return std::hash<uint64_t>()(k.lo ^ (static_cast<uint64_t>(k.more) * 0x9E3779B97F4A7C15ull));
        }
    };
    std::unordered_map<Key, VkPipeline, KeyHash> pipelines_;
    std::unordered_map<Key, std::shared_future<VkPipeline>, KeyHash> pending_;
};

}  // namespace bro::scene::vk
