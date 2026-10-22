#pragma once
// The plants' GPU plan: each frame's choice of what every plant is drawn
// with, in the camera and in each shadow view, made on the GPU by
// plants_plan.comp straight into the chunk records the plants' draws read,
// their counts into indirect commands, so a frame's CPU work does not grow
// with the plants (the drawer, scene_mesh_drawer_plants.cpp, fills the
// parameters and issues the draws; this owns the stages, the nodes' buffers
// and their read-backs, and records the frame's plans).
//
// A node's plants are a world-cell provider (world_cells.h; their cells
// PlantsNode's, in the cell order its records are kept in). Ahead of the
// plan, the cell pass (scene_world_cells_pass.h) culls the cells against
// the camera and every shadow view drawn, and the plan walks the plants of
// the cells in view alone: count each view's chunks a slot, lay the slots
// out, place each plant's chunks; then the element poses (plants_pose.comp
// with the node's world module, its PlantShaders' pose stage), dispatched
// over the leaf chunks the plan made.
//
// It records into the frame's compute stream; what the draws read (chunks,
// commands, poses) is kept a copy a frame slot, what only the plan touches
// one copy; the stats come back a frame or two late (PlantsNode::
// StatsSource), and the room for chunks and poses grows from them: a frame
// that asks for more than there is draws what fits. Each view's draws start
// at their own first chunk, an instance index from the indirect command
// (the device's drawIndirectFirstInstance, which bro requires).

#include "render/vulkan_frames.h"
#include "scene/nodes/plants_node.h"
#include "scene/vulkan/foliage/scene_plant_shaders.h"
#include "scene/vulkan/frame/scene_vk_allocator.h"
#include "scene/vulkan/world/scene_world_cells_pass.h"
#include "scene/vulkan/shaders/foliage/scene_plants_plan_layout.h"
#include "scene/world/world_cells.h"

#include <vulkan/vulkan.h>

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

namespace plants {

/// View `view`'s (0 the camera, 1 + t shadow view t as planned) leaves' or
/// wood's indirect command, its byte offset in the args buffer.
constexpr VkDeviceSize commandOffset(uint32_t view, bool wood) {
    return VkDeviceSize{PP_A_COMMANDS + (2 * view + (wood ? 1u : 0u)) * PP_A_COMMAND_WORDS} * 4u;
}

class GpuPlanner {
public:
    /// A node's plan state, kept from frame to frame.
    class Node : public PlantsNode::StatsSource {
    public:
        bool latest(PlantsNode::Stats& out) override;

        // This frame's (add()): what the draws read, this frame slot's.
        SceneVkBuffer chunks, args, poses;
        VkDescriptorSet drawSet = VK_NULL_HANDLE;   // the draws' set 3 (and the pose stage's)

    private:
        friend class GpuPlanner;
        void poll();

        static constexpr uint32_t kSlots = render::VulkanFrames::kFramesInFlight;
        SceneVkBuffer slotChunks_[kSlots], slotArgs_[kSlots], slotPoses_[kSlots];
        uint64_t used_ = 0;
        // Its cells (one provider), their GPU copy and the cell pass's output.
        WorldCells cells_;
        WorldCells::Provider provider_ = cells_.addProvider();
        uint64_t generation_ = 0;   // the node's its cells were built from
        uint64_t cellsGeneration_ = 0;
        WorldCellsPass::Target cellTarget_;
        SceneVkBuffer work_, readback_;
        VkDescriptorSet planSet_ = VK_NULL_HANDLE;
        VkPipeline pose_ = VK_NULL_HANDLE;   // this frame's pose stage (its module's)
        uint32_t askedRecords_ = 0, askedLeaves_ = 0;
        // The read-back ring: a slot a frame, stamped with its submission.
        static constexpr int kRing = 4;
        struct Slot {
            enum State : uint8_t { Free, Recorded, Submitted } state = Free;
            uint64_t ticket = 0, serial = 0;
            render::VulkanQueue* queue = nullptr;
        };
        Slot ring_[kRing];
        int cursor_ = 0;
        uint64_t serial_ = 0, readSerial_ = 0;
        bool have_ = false;
        PlantsNode::Stats newest_;
    };

    bool setup(SceneGpu& gpu);
    void cleanup(SceneGpu& gpu);

    /// Node `n`'s state (made on first use; a node long unplanned is dropped).
    std::shared_ptr<Node> node(const PlantsNode& n);

    /// Queue `state`'s plan of `node` this frame: its cells current, its
    /// buffers sized from the last frames' asks, its sets written (the
    /// draws' set 3 reading `store`, the node's records). `tiles` the
    /// shadow views its casters go to (the rects of shadow pages the frame
    /// draws), by index in the frame's plan (ShadowPlan::views). False:
    /// it draws nothing this frame (its cells, buffers or sets could not be
    /// had).
    /// Its poses are built by `shaders`' pose stage (its world's module).
    bool add(SceneFrame& frame, Node& state, const PlantsNode& node, const SceneVkBuffer& store,
             const std::vector<int>& tiles, const PlantShaders& shaders);
    /// Record the queued plans and their poses into the frame's compute stream.
    void record(SceneFrame& frame);
    /// The plans just recorded went to the GPU as `ticket` on `queue`.
    void submitted(uint64_t ticket, render::VulkanQueue& queue);

private:
    enum Stage { Count, Layout, Place, kStages };
    static void destroyNode(SceneVkAllocator& allocator, Node& n);
    bool ensure(SceneFrame& frame, SceneVkBuffer& buf, VkDeviceSize size, VkBufferUsageFlags usage,
                VkMemoryPropertyFlags props);

    VkDevice device_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkShaderModule modules_[kStages] = {};
    VkPipeline pipelines_[kStages] = {};
    VkPipelineLayout poseLayout_ = VK_NULL_HANDLE;
    std::vector<VkPipeline> posePipelines_;   // by PlantShaders::index
    WorldCellsPass cellsPass_;
    VkPhysicalDeviceLimits limits_{};

    std::unordered_map<const PlantsNode*, std::shared_ptr<Node>> nodes_;
    uint64_t use_ = 0;
    std::vector<Node*> queued_;
};

}  // namespace plants
}  // namespace bro::scene::vk
