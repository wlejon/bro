#pragma once
// The land's processes on the GPU (scene/world/land_grid.h): each land's grid, two images of
// its cells (rgba32f, a layer a root's channel, each root with its halo)
// written in turn, its processes' kernels run over it (land_process.comp,
// the land's LandProcessShaders set) a frame's budget of dispatches at a
// time in the frame's graphics stream, the halo's copies after each, and
// its first channel read back once, when the last pass is done, for the
// land's CPU heights (LandNode::setGridBase). The land is drawn and its
// heights answer from then on; its fills read the grid the last pass
// wrote.

#include "scene/vulkan/frame/scene_vk_allocator.h"
#include "scene/vulkan/frame/scene_vk_pipeline.h"
#include "scene/vulkan/ground/scene_land_shaders.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace bro::scene {
class LandNode;
}

namespace bro::scene::vk {

struct SceneFrame;
struct SceneGpu;

class SceneLandProcesses {
public:
    /// Dispatches recorded a frame at most, a pass's and its halo's each.
    static constexpr int kDispatchesPerFrame = 256;

    bool setup(SceneGpu& gpu);
    void cleanup(SceneGpu& gpu);

    /// Land `node`'s processes this frame: started over when its grid's
    /// serial changes, the frame's budget of their dispatches recorded into
    /// `cmd`, and the read-back handed to the node once it is in. The view
    /// of its grid as the land's fill reads it once every dispatch is
    /// recorded (none() for a land with no processes); else null.
    VkImageView update(SceneFrame& frame, LandNode& node, VkCommandBuffer cmd);
    /// What a fill binds for a land with no processes (1 x 1, never read).
    VkImageView none() const { return none_.view; }
    /// Land `id` is gone.
    void forget(SceneGpu& gpu, uint32_t id);
    /// The frame went to the graphics queue as `ticket`: a read-back it
    /// recorded is read once that completes.
    void submitted(uint64_t ticket);

private:
    struct Run {
        uint64_t serial = 0;
        VkPipeline pipeline = VK_NULL_HANDLE;
        SceneVkImage grids[2];
        SceneVkBuffer halo, data, readback;
        uint32_t haloCopies = 0;
        bool started = false;   // procInit recorded
        int current = 0;        // the grid the last dispatch wrote
        size_t pass = 0;        // the next pass and its iteration
        int iteration = 0;
        enum class State { Running, Copied, Reading, Done, Failed } state = State::Running;
        uint64_t ticket = 0;
    };

    /// Null while its kernels compile (`*pending` true) or when they failed.
    VkPipeline pipeline(const LandNode& node, bool* pending = nullptr);
    bool start(SceneGpu& gpu, const LandNode& node, Run& run);
    void record(SceneFrame& frame, const LandNode& node, Run& run, VkCommandBuffer cmd);
    void release(SceneGpu& gpu, Run& run);

    VkDevice device_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    SceneVkImage none_;
    // The compiled sets, by their sources' hash (-1: did not compile), and
    // a pipeline each.
    std::vector<std::unique_ptr<LandProcessShaders>> sets_;
    std::vector<AsyncComputePipeline> pipelines_;
    std::unordered_map<uint64_t, int> byHash_;
    std::unordered_map<uint32_t, std::unique_ptr<Run>> runs_;
};

}  // namespace bro::scene::vk
