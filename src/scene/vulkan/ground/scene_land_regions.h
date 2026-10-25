#pragma once
// A land's regional grids on the GPU (docs/scene-world-architecture.md §11,
// "As built: regional grids"; scene/world/land_regions.h): the regions wanted
// about the eye (landRegionsWanted) made up to kBatch of a level at a time,
// each in two images of a region's cells (rgba32f, a layer a channel), the
// world's region kernels
// (land_region.comp, the land's LandRegionShaders set) run over it a frame's
// budget of dispatches at a time in the frame's graphics stream, its kept
// channels copied into a slot of the land's region slots and its channel 0
// read back for the CPU. A region is read (in the table the fills and the
// next regions' runs look regions up in, and by the CPU's heights through the
// land's LandRegionStore) from the frame its read-back is in, on the GPU and
// the CPU at once, and the land's items under it are made stale then
// (LandNode::regionChanged), so its tiles are filled again over it.
//
// A region of a level starts only once every wanted region of the coarser
// levels that it overlaps is in (it starts from them, procCoarse; on another
// root, any), so what it is made from is what it would be made from had the
// whole coarser level come first. The regions holding the ground under the
// eye go first, each level's as soon as the coarser ones under it are in,
// then the rest nearest first. Regions no longer wanted
// stay while their slots are not needed; a region dropped takes the finer
// regions over it with it (they were made from it).

#include "scene/vulkan/frame/scene_vk_allocator.h"
#include "scene/vulkan/frame/scene_vk_pipeline.h"
#include "scene/vulkan/ground/scene_land_shaders.h"
#include "scene/world/land_regions.h"

#include "render/vulkan_frames.h"

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

class SceneLandRegions {
public:
    /// Steps offered a frame (one world-service job each: the service
    /// grants what its budget holds) once the ground about the eye has been
    /// in, and before (the first frames wait on it anyway).
    static constexpr int kDispatchesPerFrame = 128;
    static constexpr int kDispatchesWarming = 1024;
    /// Regions of one level made at once, a BATCH: each step dispatches
    /// over every region of it before the one barrier the step waits on (a
    /// pass over a 512-cell region is mostly the barrier's drain, so a batch
    /// of four costs little more than one). Each region has its own run
    /// images, so its results are its own, bit for bit as made alone.
    static constexpr int kBatch = 4;
    /// Bands of its rows a region's init (the world's noise over its cells,
    /// the dearest of its steps) is split into, a step each, so a step is
    /// never much over a pass. A pass is one step.
    static constexpr int kBands = 16;
    /// Steps a frame made whatever the budget once the ground about the eye
    /// has been in (at the budget's floor, behind work that never runs out,
    /// the regions about a moving eye never came in).
    static constexpr int kRequiredSteps = 2;
    /// Read-backs in flight at most (a region's channel 0 each).
    static constexpr int kReadbacks = 2 * kBatch;

    bool setup(SceneGpu& gpu);
    void cleanup(SceneGpu& gpu);

    /// A batch's steps, each its own world-service kind (priced apart: an
    /// init band is the world's noise, dearer than a pass, and the finish
    /// is copies and read-backs). A frame offers one kind's: the batch's
    /// init bands until they are run, then its passes, then its finish.
    enum class Step { Init, Pass, Finish };
    /// The work a land's regions offer the world service this frame: the
    /// next steps of the batch being made (none: nothing to make). While
    /// the ground under the eye is not in (`urgent`: the land is wrong
    /// there, and the plants wait on it) all of them are required, and the
    /// offerer bounds them to a frame's slice of GPU time (kUrgentMs at the
    /// steps' measured cost), so a view arrived at has its ground in a
    /// second or two rather than at the budget's floor, and no one frame
    /// runs them all (the boot's first frame ran 1024 steps, 320 ms).
    struct Offer {
        Step step = Step::Pass;
        int steps = 0;      // a job each
        int required = 0;   // the first so many of them required
        int regions = 0;    // the batch's regions: each job's units
        float error = 0.0f;   // the pixels a cell of the level being made spans
        bool urgent = false;
    };
    /// GPU ms a frame of urgent steps (the world service's settle budget's).
    static constexpr float kUrgentMs = 8.0f;
    /// Land `node`'s regions this frame, over its global grid `grid` (the
    /// view SceneLandProcesses::update gave, null while it runs): read-backs
    /// in handed to the node, the wanted regions about the frame's eye, this
    /// frame's table written into `cmd` where it changed; and what there is
    /// to make.
    Offer prepare(SceneFrame& frame, LandNode& node, VkImageView grid, VkCommandBuffer cmd);
    /// Up to `budget` of its steps of kind `step` (the world service's
    /// grant) recorded into `cmd`; the regions' steps they were (a step over
    /// the batch's n regions counting n).
    int run(SceneFrame& frame, LandNode& node, VkImageView grid, VkCommandBuffer cmd, Step step, int budget);
    /// What a land's fill binds for its regions this frame (none(): a land
    /// with none, never read).
    VkImageView slots(uint32_t id) const;
    VkDescriptorBufferInfo table(uint32_t id) const;
    /// Regions in and regions wanted not yet in.
    void counts(uint32_t id, uint32_t& in, uint32_t& pending) const;
    /// Land `id` is gone.
    void forget(SceneGpu& gpu, uint32_t id);
    /// The frame went to the graphics queue as `ticket`.
    void submitted(uint64_t ticket);

private:
    struct Resident {
        LandRegionKey key;
        int slot = -1;
        bool in = false;          // read back, in the table and the store
        uint64_t wanted = 0;      // the frame it was last wanted
    };
    struct Member {
        LandRegionKey key;
        int slot = -1;
    };
    /// The regions being made, in lockstep (member m in run[m]).
    struct Job {
        std::vector<Member> members;
        bool started = false;   // its init's bands all run
        int current = 0;
        size_t pass = 0;
        int iteration = 0;
        int band = 0;   // the next band of the init
    };
    struct Reading {
        LandRegionKey key;
        int buffer = -1;
        uint64_t ticket = 0;
        bool ticketed = false;
    };
    struct Land {
        uint64_t serial = 0;
        bool failed = false;
        VkPipeline pipeline = VK_NULL_HANDLE;
        SceneVkImage slots, run[kBatch][2];
        SceneVkBuffer data;
        SceneVkBuffer readbacks[kReadbacks];
        bool readbackBusy[kReadbacks] = {};
        std::unordered_map<uint32_t, Resident> resident;   // by key
        std::vector<int> freeSlots;
        bool hasJob = false;
        Job job;
        std::vector<Reading> reading;
        uint64_t frame = 0;
        std::vector<LandRegionKey> wanted;   // this frame's, in the order they are made
        uint32_t pending = 0;
        // The table (device-local: every texel of every fill looks regions
        // up in it), written again when the regions in change.
        // Past its entries, each level's centre (land_regions.h, PRESENCE):
        // where each level is read about now, kept by its fade.
        SceneVkBuffer tableBuffer;
        bool tableDirty = true;
        LandRegionFade fades[LandRegionConfig::kMaxLevels];
        LandRegionCentre centres[LandRegionConfig::kMaxLevels];
        VkDescriptorBufferInfo table{};
    };

    /// Null while its kernels compile (`*pending` true) or when they failed.
    VkPipeline pipeline(const LandNode& node, bool* pending = nullptr);
    bool start(SceneGpu& gpu, const LandNode& node, Land& land);
    void release(SceneGpu& gpu, Land& land);
    void receive(SceneFrame& frame, LandNode& node, Land& land);
    void drop(LandNode& node, Land& land, uint32_t packed);
    /// The next batch begun: the first region of `near` (those holding the
    /// ground under the eye), else of `wanted`, whose coarser regions are in,
    /// and the regions of its level after it in `wanted` whose are too.
    bool begin(LandNode& node, Land& land, const std::vector<LandRegionKey>& wanted,
               const std::vector<LandRegionKey>& near);
    /// The batch's next step: its init's bands run, its passes run (the
    /// finish is all that is left), or neither.
    static Step stepOf(const LandRegionConfig& C, Job& job);
    int freeReadbacks(const Land& land) const;
    void record(SceneFrame& frame, LandNode& node, Land& land, VkImageView grid, VkCommandBuffer cmd, Step step,
                int& budget);
    void writeTable(SceneFrame& frame, const LandNode& node, Land& land, VkCommandBuffer cmd);

    VkDevice device_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    SceneVkImage none_;
    SceneVkBuffer noneTable_;
    std::vector<std::unique_ptr<LandRegionShaders>> sets_;
    std::vector<AsyncComputePipeline> pipelines_;
    std::unordered_map<uint64_t, int> byHash_;
    std::unordered_map<uint32_t, std::unique_ptr<Land>> lands_;
};

}  // namespace bro::scene::vk
