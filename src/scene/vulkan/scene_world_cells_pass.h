#pragma once
// The GPU cell pass (docs/scene-world-architecture.md §1): a provider's
// world cells (world_cells.h) culled against a frame's views, the camera's
// frustum and its shadow tiles', top-down in three dependent dispatches
// (world_cells_cull.comp: the tops; the leaves of the tops in view, a
// workgroup a top; the cells in view gathered into one list in order).
// It writes, in the cells' order, the cells some view sees with the views
// that see each, its eye-relative origin, its size in pixels in each view
// and its band by the provider's thresholds (near, mid, far), each band's
// list, the counts, and an indirect dispatch over the items in view
// (shaders/world_cells_layout.h), which the provider's own passes walk
// instead of all their items (world_cells.h has the provider contract). Recorded on the frame's compute stream, ahead
// of what reads it.
//
// Frustum only for now: the hierarchical depth of a visibility pass (§3)
// joins the same test later. A target's output is one copy, as the forest
// planner's work is: the compute queue orders a frame's pass after the last
// frame's reads.

#include "scene/vulkan/scene_vk_allocator.h"
#include "scene/world_cells.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace bro::scene::vk {

struct SceneFrame;
struct SceneGpu;

class WorldCellsPass {
public:
    /// One provider's cells on the GPU, and the pass's output for them.
    struct Target {
        SceneVkBuffer cells, out;
        VkDescriptorSet set = VK_NULL_HANDLE;   // this frame's (begin())
        uint64_t generation = ~uint64_t{0};     // what `cells` was built from
        uint32_t tops = 0, leaves = 0;
    };

    /// How a view turns metres into pixels (WC_SCALE_*): a perspective
    /// camera by its focal length in pixels, a shadow tile by its texel.
    struct Scale {
        float focalPixels = 0.0f;      // > 0: a camera
        float texel = 0.0f;            // a tile's texel, metres
        float texelPerMetre = 0.0f;    // its growth a metre from the eye (0 ortho)
    };

    /// A frame's views: the camera first. `planes` 24 words a view (each
    /// plane's normal xyz and d, as floats' bits), the same words the
    /// provider tests its items with; `scales` one a view. The bands are
    /// the provider's: a cell whose camera size is `nearPixels` or more is
    /// near, one under `farPixels` far.
    struct Views {
        double eye[3] = {0.0, 0.0, 0.0};   // the camera's (origins in double; distances in float)
        uint32_t count = 0;
        const uint32_t* planes = nullptr;
        const Scale* scales = nullptr;
        float nearPixels = 0.0f, farPixels = 0.0f;
        uint32_t itemGroup = 256;   // items a workgroup of the provider's dispatch
    };

    bool setup(SceneGpu& gpu);
    void cleanup(SceneGpu& gpu);
    bool ready() const { return ready_; }
    static void destroy(SceneVkAllocator& allocator, Target& target);

    /// `target`'s cells current: provider `p`'s of `cells`, uploaded again
    /// when `generation` moves (the planner's waits for the upload).
    bool update(SceneFrame& frame, Target& target, const WorldCells& cells, WorldCells::Provider p,
                uint64_t generation);
    /// This frame's views for `target` (its params, its leaves' eye-relative
    /// origins from provider `p`'s of `cells`, and its set).
    bool begin(SceneFrame& frame, Target& target, const Views& views, const WorldCells& cells,
               WorldCells::Provider p);
    /// Record the pass for `targets` (each begun this frame): every top
    /// step, then every leaf step, then every gather. The caller's barrier
    /// orders its readers after.
    void record(VkCommandBuffer cmd, const std::vector<Target*>& targets) const;

private:
    VkDevice device_ = VK_NULL_HANDLE;
    bool ready_ = false;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkShaderModule modules_[3] = {};
    VkPipeline pipelines_[3] = {};
    VkDeviceSize uploadAlign_ = 16;
};

}  // namespace bro::scene::vk
