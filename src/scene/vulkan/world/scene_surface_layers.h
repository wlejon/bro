#pragma once
// The GPU half of a surface layer set (scene/world/surface_layers.h): its
// tiles' atlas, one 2D-array image a
// layer with an array layer a slot (each slot a tile with its border), and
// its page table, a 2D-array R32_UINT image with a layer a level. It moves
// the slots a frame fills between sampling and being written, writes nothing
// itself (the provider draws each fill into slotView()s, or with
// Fill::Compute stores into the layers' whole views from a compute shader)
// and writes the page table's changed entries, all in the frame's own
// graphics stream, after whatever read them in earlier frames: so no fill
// waits for a reader. Its readers are vertex and fragment stages.
//
// A compute-filled atlas's layers stay in GENERAL, so one array view serves
// both the fills' storage writes and the draws' sampling.

#include "scene/world/surface_layers.h"
#include "scene/vulkan/frame/scene_vk_allocator.h"
#include "scene/vulkan/frame/scene_vk_target_format.h"

#include <vulkan/vulkan.h>

#include <array>
#include <span>
#include <vector>

namespace bro::scene::vk {

class SceneVkDevice;

class SceneSurfaceLayerAtlas {
public:
    static constexpr int kMaxLayers = static_cast<int>(TargetFormat::kMaxColor);

    /// How the provider fills a slot: rendering into it, or storing into it
    /// from a compute shader.
    enum class Fill { Render, Compute };

    /// Images for `config` (its slots, its tile's stride) with `levels`
    /// page-table levels and a layer per format (at most kMaxLayers).
    bool create(SceneVkDevice& device, SceneVkAllocator& allocator, const SurfaceLayerConfig& config, int levels,
                std::span<const VkFormat> formats, Fill fill = Fill::Render);
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

    /// Before the fills: their slots' layers into COLOR_ATTACHMENT (into
    /// writes from a compute shader, Fill::Compute), after every earlier read
    /// of them.
    void beginFills(VkCommandBuffer cmd, std::span<const SurfaceFill> fills) const;
    /// Before each fill: the depth tile, its last contents discarded (Fill::Render).
    void beginDepth(VkCommandBuffer cmd) const;
    /// After the fills: their slots ready for the frame's draws to read.
    void endFills(VkCommandBuffer cmd, std::span<const SurfaceFill> fills) const;
    /// Write the changed entries (staged in the frame's upload memory),
    /// after every earlier read of the table.
    void writePages(SceneVkDevice& device, VkCommandBuffer cmd, std::span<const SurfacePageWrite> writes);
    /// Every entry to none (the set was made or reconfigured: no tile is
    /// kept, so neither are the layers' contents).
    void clearPages(VkCommandBuffer cmd);

    /// The stages that read the layers and the page table.
    static constexpr VkPipelineStageFlags kReaders =
        VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;

private:
    void slotBarrier(VkCommandBuffer cmd, std::span<const SurfaceFill> fills, bool toTarget) const;

    std::array<SceneVkImage, kMaxLayers> layers_{};
    std::array<std::vector<VkImageView>, kMaxLayers> slotViews_{};
    SceneVkImage page_{};
    SceneVkImage depth_{};
    VkSampler linear_ = VK_NULL_HANDLE;
    VkSampler nearest_ = VK_NULL_HANDLE;
    int layerCount_ = 0;
    Fill fill_ = Fill::Render;
    int slots_ = 0;
    int levels_ = 0;
    int pageSize_ = 0;
    uint32_t stride_ = 0;
};

}  // namespace bro::scene::vk
