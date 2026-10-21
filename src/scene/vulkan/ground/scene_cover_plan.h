#pragma once
// The cover's plan: each frame, the blades and herb plants a grass node's
// rings draw, chosen on the GPU (shaders/ground/cover_plan.comp). A ring's
// cells in view are runs of tiles (scene_mesh_drawer_grass.cpp); every
// candidate in them (a blade of a cell, a cell of the herbs' grid) is tested
// once by the test its vertices used to run each (grassBladeLive,
// herbPlantLive), and the ones drawn are listed for the ring's draw, which
// takes them as its instances through an indirect command. So a ring's
// vertices run only for what it draws: the rings issued some 5 million
// vertices a frame at eye level for well under a million triangles on
// screen.
//
// Recorded in the frame's graphics stream ahead of the passes (it samples
// the node's field textures, which live on the graphics queue). A node's
// list and commands are kept a copy a frame slot, sized each frame to its
// candidates, so nothing is read back.

#include "render/vulkan_frames.h"
#include "scene/vulkan/frame/scene_vk_allocator.h"

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace bro::scene::vk {

struct SceneFrame;
struct SceneGpu;

class CoverPlan {
public:
    /// One ring's share of a node's plan.
    struct Ring {
        /// Its cells: runs of (first column, row, columns, rows).
        const std::vector<std::array<int, 4>>* runs = nullptr;
        uint32_t perCell = 1;     // candidates a cell: its blades, or a herb cell's one plant
        bool herbs = false;
        VkDescriptorSet set = VK_NULL_HANDLE;   // the ring's grass set (set 4)
        uint32_t indexCount = 0;  // what its draw draws an instance
    };
    /// What a ring's draw takes: its indirect command, and the set (set 3)
    /// holding the list its instances read.
    struct Draw {
        VkBuffer commands = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;
        VkDescriptorSet listSet = VK_NULL_HANDLE;
    };

    bool setup(SceneGpu& gpu);
    void cleanup(SceneGpu& gpu);

    /// Plan node `nodeId`'s `rings` this frame: `out[i]` is ring i's draw.
    /// False: it draws none of them this frame (its buffers or sets could
    /// not be had).
    bool add(SceneFrame& frame, uint32_t nodeId, std::span<const Ring> rings, std::vector<Draw>& out);
    /// Record the frame's plans into its graphics stream, outside any
    /// rendering and before the passes that draw them.
    void record(SceneFrame& frame);

private:
    struct NodeState {
        static constexpr uint32_t kSlots = render::VulkanFrames::kFramesInFlight;
        SceneVkBuffer list[kSlots], commands[kSlots];
        uint64_t used = 0;
    };
    /// A ring's dispatch.
    struct Job {
        VkDescriptorSet listSet = VK_NULL_HANDLE, set = VK_NULL_HANDLE;
        uint32_t firstRun = 0, runCount = 0, perCell = 1, candidates = 0, count = 0, first = 0, herbs = 0;
    };
    /// A node's commands, as they start each frame.
    struct Commands {
        VkBuffer buffer = VK_NULL_HANDLE;
        std::vector<uint32_t> words;
    };
    static void destroy(SceneVkAllocator& allocator, NodeState& n);

    VkDevice device_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkDeviceSize storageAlign_ = 16;

    std::unordered_map<uint32_t, NodeState> nodes_;
    uint64_t use_ = 0;
    std::vector<Job> jobs_;
    std::vector<Commands> commands_;
};

}  // namespace bro::scene::vk
