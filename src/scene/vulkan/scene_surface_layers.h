#pragma once
// The GPU half of a surface layer set (scene/surface_layers.h, docs/
// scene-world-architecture.md §5): its tiles' atlas, one 2D-array image a
// layer with an array layer a slot (each slot a tile with its border), and
// its page table, a 2D-array R32_UINT image with a layer a level. It moves
// the slots a frame fills between sampling and rendering, renders nothing
// itself (the provider draws each fill into slotView()s) and writes the page
// table's changed entries, all in the frame's own graphics stream, after
// whatever read them in earlier frames: so no fill waits for a reader.

#include "scene/surface_layers.h"
#include "scene/vulkan/scene_vk_allocator.h"
#include "scene/vulkan/scene_vk_target_format.h"

#include <vulkan/vulkan.h>

#include <array>
#include <span>
#include <vector>

namespace bro::scene::vk {

class SceneVkDevice;

class SceneSurfaceLayerAtlas {
public:
    static constexpr int kMaxLayers = static_cast<int>(TargetFormat::kMaxColor);

    /// Images for `config` (its slots, its tile's stride) with `levels`
    /// page-table levels and a layer per format (at most kMaxLayers).
    bool create(SceneVkDevice& device, SceneVkAllocator& allocator, const SurfaceLayerConfig& config, int levels,
                std::span<const VkFormat> formats);
    void destroy(SceneVkDevice& device, SceneVkAllocator& allocator);
    bool valid() const { return layerCount_ > 0; }
    /// Built for this many slots, this stride and levels.
    bool fits(const SurfaceLayerConfig& config, int levels) const;

    int layerCount() const { return layerCount_; }
    VkImageView layerView(int layer) const { return layers_[layer].view; }
    VkSampler layerSampler() const { return linear_; }
    VkImageView pageView() const { return page_.view; }
    VkSampler pageSampler() const { return nearest_; }

    /// What a fill renders into: the layers' formats and a depth tile.
    TargetFormat target() const;
    /// Layer `layer` of slot `slot`, a single-layer view to render into.
    VkImageView slotView(SceneVkDevice& device, int layer, int slot);
    const SceneVkImage& depth() const { return depth_; }
    uint32_t stride() const { return stride_; }

    /// Before the fills: their slots' layers into COLOR_ATTACHMENT, after
    /// every earlier read of them.
    void beginFills(VkCommandBuffer cmd, std::span<const SurfaceFill> fills) const;
    /// Before each fill: the depth tile, its last contents discarded.
    void beginDepth(VkCommandBuffer cmd) const;
    /// After the fills: their slots back to SHADER_READ_ONLY for the frame's draws.
    void endFills(VkCommandBuffer cmd, std::span<const SurfaceFill> fills) const;
    /// Write the changed entries (staged in the frame's upload memory),
    /// after every earlier read of the table.
    void writePages(SceneVkDevice& device, VkCommandBuffer cmd, std::span<const SurfacePageWrite> writes);
    /// Every entry to none (the set was reconfigured).
    void clearPages(VkCommandBuffer cmd);

private:
    void slotBarrier(VkCommandBuffer cmd, std::span<const SurfaceFill> fills, bool toTarget) const;

    std::array<SceneVkImage, kMaxLayers> layers_{};
    std::array<std::vector<VkImageView>, kMaxLayers> slotViews_{};
    SceneVkImage page_{};
    SceneVkImage depth_{};
    VkSampler linear_ = VK_NULL_HANDLE;
    VkSampler nearest_ = VK_NULL_HANDLE;
    int layerCount_ = 0;
    int slots_ = 0;
    int levels_ = 0;
    int pageSize_ = 0;
    uint32_t stride_ = 0;
};

}  // namespace bro::scene::vk
