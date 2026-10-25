#pragma once

// The cloud volume about the eye (sky/clouds_layout.h's CL_VOL_*;
// docs/clouds-api.js, cloudStats().volume): the engine's forms as the march
// draws them (sky/cloud_forms_at.glsl's clFormsAt, from the cache's base),
// baked into a clipmap of 3D grids about the eye, so a march can read a cell
// where it evaluated the forms (their extinction at the cell's footprint),
// jump through clear air by a distance it may trust (to the nearest cell
// that may hold cloud as the forms draw it at any footprint up to the
// cell's), and light a sample by one fetch (the optical depth toward the
// sun). SceneClouds owns it and binds its images in the clouds' set.
//
//   - The frame: a tangent frame at an anchor on the deck's bottom near the
//     eye, fixed in the clouds' drifted frame (cloudDrift), so the wind
//     moves no cell; made again (every level baked again) when the eye is
//     kAnchorReach from the anchor, as the deck curves away from its plane.
//   - The levels: CL_VOL_LEVELS windows of CL_VOL_SIDE^2 x CL_VOL_DEPTH
//     cells, level k's cell CL_VOL_CELL0 * 2^k, about the eye's cell (a
//     quarter cell of hysteresis), vertically about the deck where the
//     window is deeper than it, else about the eye held in the deck;
//     stored toroidally, so a window that moves bakes only what it takes
//     in (and the distances out to the reach about it and its given-up edge).
//   - The bakes (sky/clouds_volume*.comp): the forms and the occupancy, the
//     distance an axis a pass, the sun's depth; a level whole when the deck,
//     the anchor or the time jumps (all of them the first frame, else one a
//     frame, coarsest first), and every frame CL_VOL_SIDE / CL_VOL_SLICES
//     rows of every level (the world's base drifting under the forms, the
//     sun moving). The distance a march reads is less the slack the rows'
//     age leaves (the wind times the time since the oldest was baked).
//   - The check (BRO_CLOUD_VOLUME_CHECK=1; sky/clouds_volume_check.comp):
//     every kCheckEvery frames, rays from the eye walked on the volume; its
//     extinction against the forms', and the forms the march would draw
//     inside each jump (violations), read back into cloudStats().
//   - The debug views (BRO_CLOUD_VOLUME_DEBUG=<view>[:<height m>]): the
//     march drawn with the CloudsVolumeFrag variant (sky/
//     cloud_volume_debug.glsl): 1 the volume instead of the forms, 2 its
//     channels as slices from above at a height, 3 the same through the
//     eye's row.

#include "scene/vulkan/frame/scene_vk_allocator.h"
#include "scene/vulkan/shaders/sky/clouds_layout.h"
#include "scene/world/clouds.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace bro::scene::vk {

struct SceneGpu;
class SceneVkDescriptorWriter;

/// The volume's part of the clouds' block (CloudsUBO's vol*,
/// sky/scene_cloud_layer.glsl).
struct CloudVolumeGpu {
    float A[4];
    float E1[4];
    float E2[4];
    float N[4];
    float eye[4];
    float level[CL_VOL_LEVELS][4];
};

class SceneCloudVolume {
public:
    bool setup(SceneGpu& gpu, VkPipelineLayout computeLayout);
    void cleanup(SceneGpu& gpu);
    bool ensureImages(SceneGpu& gpu);
    /// Its bindings of the clouds' set.
    void write(SceneVkDescriptorWriter& w) const;

    /// What the frame is: the eye (the body's frame, m), the wind and its
    /// time, the deck's radii, a pixel's angle, the deck's key (module,
    /// generation, shader epoch) and whether the deck or its time jumped.
    struct Frame {
        double eye[3];
        float wind[3];
        float windTime;
        float Rb, Rt;
        float pixel;
        uint64_t key;
        bool jump;
        bool sourceSettled;   // the cache it is baked from has no bakes to come
    };
    /// Where the levels lie this frame and what of them is baked; their
    /// place in the block, the settle state and the check into `stats`.
    void place(const Frame& f, CloudVolumeGpu& out, CloudStats& stats);
    /// The bakes (and the check), into the compute stream with the clouds'
    /// set `set` (its block holds this frame's placement).
    void record(VkCommandBuffer cmd, VkDescriptorSet set, VkPipelineLayout layout);
    void submitted(uint64_t ticket);

    /// The march's debug view (0 none) and the slice's height.
    static int debugView();
    static float debugHeight();

private:
    struct Box {
        int k;
        int x0, nx, y0, ny, z0, nz;
        int pass0;   // 0 the forms on, 1 the distance on
    };
    void moveLevel(int k, int axis, long long from, long long to);
    void readCheck(CloudStats& stats);

    SceneVkDevice* device_ = nullptr;
    VkPipeline forms_ = VK_NULL_HANDLE, dist_ = VK_NULL_HANDLE, sun_ = VK_NULL_HANDLE, check_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    SceneVkImage vol_[CL_VOL_LEVELS], distA_[CL_VOL_LEVELS], distB_[CL_VOL_LEVELS];
    SceneVkBuffer checkBuf_;

    // The frame (the drifted body frame) and the levels' windows.
    bool anchored_ = false;
    double A_[3] = {}, E1_[3] = {}, E2_[3] = {}, N_[3] = {};
    long long centre_[CL_VOL_LEVELS][3] = {};
    bool baked_ = false;            // every level holds the deck (the first bake is whole)
    uint32_t pending_ = 0;          // levels to bake whole again, a level a frame
    uint64_t key_ = 0;
    uint32_t frame_ = 0;
    int sliceRun_ = 0;              // slices made since the last whole change or change of the cache
    float sliceTime_[CL_VOL_SLICES] = {};   // the wind's time each slice of rows was last baked at
    std::vector<Box> boxes_;
    uint64_t cellsBaked_ = 0;

    // The check: a serial, the buffer in flight and its ticket.
    uint32_t checkSerial_ = 0;
    bool checkRecorded_ = false, checkPending_ = false;
    uint64_t checkTicket_ = 0;
    CloudStats::VolumeCheck lastCheck_{};
};

}  // namespace bro::scene::vk
