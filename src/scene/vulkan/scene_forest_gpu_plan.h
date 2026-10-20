#pragma once
// The forest's GPU planner: the CPU planner's choice of every tree's tiers,
// fades, bark and entries (scene_mesh_drawer_forest.cpp), made on the GPU
// by the forest_plan_*.comp stages (scene_forest_plan.glsl) straight into
// the entries the forest's draws read, their counts into indirect
// commands. The drawer (scene_mesh_drawer_forest_gpu.cpp) fills each node's
// parameters and its draws; this owns the stages, the nodes' buffers and
// their read-backs, and records the frame's plan.
//
// A frame's nodes share one budget (one search, one settled detail), as
// prepareForests' do on the CPU. Their stats come back a frame or two late
// (ForestNode::StatsSource), and the room for entries and poses grows from
// them: a frame that asks for more than there is draws what fits (the
// entries past the room are dropped, the count still says all).
//
// The plan records into the frame's compute stream (SceneFrame::
// computeStream): on the async compute queue when there is one, beside the
// frame's graphics up to its first forest draw. What it writes for the draws
// (entries, indirect commands, and the drawer's leaf poses) is kept a copy a
// frame slot, so a frame's plan never waits for the last frame's draws to
// finish with them; what only the plan reads and writes (its work, the
// shared search and settled details, the read-backs) is one copy, ordered by
// the compute queue itself, so its state carries from frame to frame as
// before. A frame whose plan reads something just uploaded on the graphics
// queue (a node's static data, its leaf store) waits for that queue first.
//
// A node's trees are a world-cell provider (world_cells.h): each leaf
// cell's run of its cell order, bounded by its trees' boxes. Ahead of the
// plan, the cell pass (scene_world_cells_pass.h) culls its cells against the
// camera and every shadow tile planned, and the measure stage walks the
// trees of the cells in view alone, the stages after it what it measured.
// The plan is the same as over every tree (a tree in a view is in a cell
// in that view); BRO_WORLD_CELLS=0 walks every tree, for A/B.

#include "render/vulkan_frames.h"
#include "scene/forest_node.h"
#include "scene/vulkan/scene_mesh_drawer.h"
#include "scene/vulkan/scene_vk_allocator.h"
#include "scene/vulkan/scene_world_cells_pass.h"
#include "scene/vulkan/shaders/scene_forest_plan_layout.h"
#include "scene/world_cells.h"

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace bro::render {
class VulkanQueue;
}

namespace bro::scene::vk {

struct SceneFrame;
struct SceneGpu;

namespace forest {

/// A view's draw slot `slot` (a group, or FP_GROUPS + j a posed group's
/// inline twin) in the args buffer: its whole-range command's byte offset.
/// View 0 is the camera, 1 + t shadow tile t as planned.
constexpr VkDeviceSize argsCommandOffset(uint32_t view, uint32_t slot) {
    return VkDeviceSize{(view * FP_A_SLOTS_PER_VIEW + slot) * FP_A_SLOT_WORDS} * 4u;
}
constexpr uint32_t kArgsBatchStride = FP_A_COMMAND_WORDS * 4u;   // bytes between a slot's commands

/// Whether every planner leaves out the cells SceneFrame::forestCellSkip
/// names: the CPU's always, the GPU's while it walks the world cells (not
/// under BRO_WORLD_CELLS=0).
bool cellSkipHonoured();

class GpuPlanner {
public:
    /// A node's planner state, kept from frame to frame: its trees and
    /// specimens (static), working words, entries, indirect commands and
    /// the read-backs of its stats.
    class Node : public ForestNode::StatsSource {
    public:
        bool latest(ForestNode::Stats& out) override;
        /// The leaves the newest read-back's posed groups asked to pose.
        uint32_t askedPoses() const { return askedPoses_; }

        // This frame's (begin()): entries and args are this frame slot's.
        VkDescriptorSet set = VK_NULL_HANDLE;   // the plan's set 0 (the pose stage's set 1)
        SceneVkBuffer entries, args;
        uint32_t entryCap = 0;

    private:
        friend class GpuPlanner;
        void poll();

        static constexpr uint32_t kSlots = render::VulkanFrames::kFramesInFlight;
        SceneVkBuffer slotEntries_[kSlots], slotArgs_[kSlots];
        uint32_t slotEntryCap_[kSlots] = {};
        VkBuffer store_ = VK_NULL_HANDLE;   // the leaf store the last plan read
        uint64_t used = 0;
        // What static was built from.
        uint64_t generation = ~uint64_t{0};
        float world[16] = {};
        float reach = -1.0f;
        size_t trees = 0;
        std::vector<uint32_t> specimenWords;
        SceneVkBuffer staticBuf, work, readback;
        // Its trees' cells (one provider), their GPU copy and the cell
        // pass's output; `cellsOn` this frame's plan walks them.
        WorldCells cells_;
        WorldCells::Provider provider_ = cells_.addProvider();
        uint64_t cellsGeneration_ = 0;
        WorldCellsPass::Target cellTarget_;
        bool cellsOn = false;
        // This frame's layout of the work buffer.
        uint32_t tiles = 0, aligned = 0, items = 0, itemGroups = 0;
        uint32_t runs[FP_W_RUNS] = {};
        // The read-back ring: a slot a frame, stamped with its submission.
        static constexpr int kRing = 4;
        struct Slot {
            enum State : uint8_t { Free, Recorded, Submitted } state = Free;
            uint64_t ticket = 0, serial = 0;
            render::VulkanQueue* queue = nullptr;   // the queue `ticket` is of
        };
        Slot ring_[kRing];
        int cursor_ = 0;
        int slotThisFrame_ = -1;
        uint64_t serial_ = 0, readSerial_ = 0;
        bool have_ = false;
        ForestNode::Stats newest_;
        uint32_t askedEntries_ = 0, askedPoses_ = 0;
    };

    bool setup(SceneGpu& gpu);
    void cleanup(SceneGpu& gpu);
    bool ready() const { return ready_; }

    /// Node `n`'s state (made on first use; a node long unplanned is dropped).
    std::shared_ptr<Node> node(const ForestNode& n);

    /// Ready `state` for this frame: its static data current (rebuilt when
    /// the node's data, transform or reach moved, or a specimen's words),
    /// its buffers sized for its trees and `tiles` shadow tiles, its entries
    /// grown to the last frames' asks, its descriptor set made. `bounds`:
    /// every tree's world box padded by `reach` (ForestCull::bounds); `store`
    /// the node's leaf store (the pose stage's). Fills the work runs and room
    /// in `params`; false on failure (the frame then goes the CPU's way).
    bool begin(SceneFrame& frame, Node& state, const ForestNode& node, VkBuffer store,
               const std::vector<bromath::AABB3>& bounds, float reach, uint32_t tiles, std::vector<uint32_t>& params);
    /// Queue `state`'s plan with its finished `params` (FP_P_WORDS words).
    /// `cellSkip`: a word a cell of the node (SceneFrame::forestCellSkip),
    /// nonzero where a far card stands for the cell's trees, which the
    /// camera then leaves out; honoured when the plan walks the cells (as
    /// cellSkipHonoured() says), else to be null.
    bool add(SceneFrame& frame, Node& state, const std::vector<uint32_t>& params,
             const std::vector<uint32_t>* cellSkip = nullptr);
    /// A posed leaf group's pose stage (forest_leaf_pose.comp's indirect
    /// build): `push` its draw's, `group` 0 LeafNear or 1 LeafCard, `boneSet`
    /// the draw's set 3.
    void addPoseJob(Node& state, const MeshPushConstants& push, uint32_t group, VkDescriptorSet boneSet);

    /// Drop this frame's queued plans and pose stages (the frame went the
    /// CPU's way after all).
    void cancel() {
        queued_.clear();
        poseJobs_.clear();
    }
    /// Record the queued plans (then their pose stages) into the frame's
    /// compute stream.
    void record(SceneFrame& frame);
    /// The plans just recorded went to the GPU as `ticket` on `queue`.
    void submitted(uint64_t ticket, render::VulkanQueue& queue);

private:
    enum Stage {
        Measure, Classify, ScanLocal, ScanAdd, SortHist, SortScatter, Layout, Items, Cost, Vary, Settle, Count, Args, Write,
        kStages
    };
    struct Push {
        uint32_t round = 0, offset = 0, count = 0, scratch = 0xffffffffu;
    };
    void dispatch(VkCommandBuffer cmd, Stage s, const Node& n, uint32_t groups, const Push& p) const;
    void dispatchItems(VkCommandBuffer cmd, Stage s, const Node& n, const Push& p) const;
    // (Overloads rather than `= {}`: GCC will not take a default argument
    // made from a nested class's member initializers inside its class.)
    void dispatch(VkCommandBuffer cmd, Stage s, const Node& n, uint32_t groups) const {
        dispatch(cmd, s, n, groups, Push{});
    }
    void dispatchItems(VkCommandBuffer cmd, Stage s, const Node& n) const { dispatchItems(cmd, s, n, Push{}); }
    // Over the groups ARGS holds at `argsWord` (an x, 1, 1 dispatch), or
    // `buffer` at byte `offset`.
    void dispatchIndirect(VkCommandBuffer cmd, Stage s, const Node& n, uint32_t argsWord, const Push& p) const;
    void dispatchIndirect(VkCommandBuffer cmd, Stage s, const Node& n, VkBuffer buffer, VkDeviceSize offset,
                          const Push& p) const;
    // Each queued node's WORK run `run` scanned, of a length the GPU set,
    // at most `most[i]` long for the i-th.
    void scanSized(VkCommandBuffer cmd, uint32_t run, uint32_t words, uint32_t args,
                   const std::vector<uint32_t>& most) const;
    static void barrier(VkCommandBuffer cmd);
    static void destroyNode(SceneVkAllocator& allocator, Node& n);
    bool ensureBuffer(SceneFrame& frame, SceneVkBuffer& buf, VkDeviceSize size, VkBufferUsageFlags usage,
                      VkMemoryPropertyFlags props, VkDeviceSize headroom, bool shared = false);
    bool buildStatic(SceneFrame& frame, Node& state, const ForestNode& node, const std::vector<bromath::AABB3>& bounds,
                     float reach);

    VkDevice device_ = VK_NULL_HANDLE;
    bool ready_ = false;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    std::array<VkShaderModule, kStages> modules_{};
    std::array<VkPipeline, kStages> pipelines_{};
    VkPipelineLayout poseLayout_ = VK_NULL_HANDLE;
    VkShaderModule poseModule_ = VK_NULL_HANDLE;
    VkPipeline posePipeline_ = VK_NULL_HANDLE;
    SceneVkBuffer shared_;
    bool sharedReady_ = false;
    WorldCellsPass cellsPass_;
    VkPhysicalDeviceLimits limits_{};

    std::unordered_map<const ForestNode*, std::shared_ptr<Node>> nodes_;
    uint64_t use_ = 0;
    std::vector<Node*> queued_;   // this frame's plans, in order
    struct PoseJob {
        Node* node = nullptr;
        MeshPushConstants push{};
        uint32_t group = 0;
        VkDescriptorSet boneSet = VK_NULL_HANDLE;
    };
    std::vector<PoseJob> poseJobs_;
};

}  // namespace forest
}  // namespace bro::scene::vk
