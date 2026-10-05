#pragma once

// What a pipeline must agree with about the attachments it draws into:
// formats, count and sample count. A pass that can draw into more than one
// target (the HDR scope at 1x or MSAA, with or without the indirect-light
// attachment; a probe face at 1x) keeps one pipeline per TargetFormat in a
// PipelineVariants map instead of rebuilding when the target changes.

#include "scene/vulkan/scene_vk_device.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>
#include <unordered_map>

namespace bro::scene::vk {

struct TargetFormat {
    static constexpr uint32_t kMaxColor = 2;

    VkFormat color[kMaxColor] = {VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED};
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
    bool operator==(const TargetFormat& o) const { return key() == o.key(); }
    bool operator!=(const TargetFormat& o) const { return key() != o.key(); }
};

/// Pipelines of one pass keyed by (variant, TargetFormat), built on first use
/// and destroyed with the pass. `variant` is the pass's own small index
/// (blend mode, vertex layout, ...), below 2^16.
class PipelineVariants {
public:
    VkPipeline get(uint32_t variant, const TargetFormat& target,
                   const std::function<VkPipeline()>& build) {
        const uint64_t key = target.key() | (static_cast<uint64_t>(variant) << 48);
        auto it = pipelines_.find(key);
        if (it != pipelines_.end()) return it->second;
        VkPipeline p = build();
        if (p != VK_NULL_HANDLE) pipelines_.emplace(key, p);
        return p;
    }

    /// Destroy every pipeline once the GPU is done with frames using them.
    void destroy(SceneVkDevice& device) {
        if (pipelines_.empty()) return;
        VkDevice dev = device.device();
        device.defer([dev, ps = std::move(pipelines_)] {
            for (auto& [k, p] : ps) vkDestroyPipeline(dev, p, nullptr);
        });
        pipelines_.clear();
    }

private:
    std::unordered_map<uint64_t, VkPipeline> pipelines_;
};

}  // namespace bro::scene::vk
