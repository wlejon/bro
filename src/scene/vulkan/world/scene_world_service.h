#pragma once
// The world service's executor: runs
// the jobs the policy (scene/world/world_service.h) grants each frame, in
// the stream each kind's schedule names, and times them, so the policy
// spends milliseconds it measured rather than guessed.
//
// A frame (PassWorldService, at the frame's head after its uniforms):
// beginFrame() hands the policy the renders the GPU timer has read since the
// last one (their milliseconds and the service's spans in them) and starts
// its frame; each producer offers its jobs; the policy ranks and grants;
// then each kind's producer records its granted jobs (run()). One code
// path, two schedules:
//  - a raster kind records into the frame's graphics stream, at its head,
//    so the frame's draws read what it made without a wait;
//  - a compute kind records into the frame's compute stream: the async
//    compute queue where the device has one (submitted ahead of the frame's
//    graphics, which waits on its timeline before its first reader: a pass
//    reading a compute producer's results declares PassIO::readsCompute),
//    else the graphics stream itself at the same point, in the same order.
// A kind's jobs are stamped as one span of the GPU timer (SceneGpuTimer::
// beginSpan) around what its producer recorded, with the units of work it
// held, so the kind's cost a unit is learned from the device. A compute
// kind's jobs share the async queue's one priority.

#include "scene/world/world_service.h"

#include <bromath/vec.h>
#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace bro::scene::vk {

struct SceneFrame;
class SceneGpuTimer;

/// The streams a frame's jobs record into.
struct WorldStreams {
    VkCommandBuffer graphics = VK_NULL_HANDLE;
    /// The frame's compute stream, opened on first use (the graphics stream
    /// itself without an async compute queue).
    std::function<VkCommandBuffer()> compute;
    SceneGpuTimer* timer = nullptr;   // null: untimed
};

/// One kind's stream this frame, as its producer records into it.
class WorldStream {
public:
    /// The command buffer to record the kind's jobs into, opened (and the
    /// kind's span begun) on the first call, outside any rendering.
    VkCommandBuffer cmd();
    /// The jobs record on the async compute queue, beside the frame's graphics.
    bool apart() const { return apart_; }

private:
    friend class SceneWorldService;
    WorldStreams* streams_ = nullptr;
    int kind_ = 0;
    WorldSchedule schedule_ = WorldSchedule::Raster;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    bool apart_ = false;
    bool stamped_ = false;
};

/// A producer of world work: it offers its jobs each frame and records
/// the ones granted.
class WorldProducer {
public:
    virtual ~WorldProducer() = default;
    /// This frame's jobs, offered to `service` under the kinds it registered.
    virtual void offer(SceneFrame& frame, WorldService& service) = 0;
    /// Kind `kind`'s granted jobs (rank order) recorded into `stream`; the
    /// units of the jobs it recorded. Called every frame for each kind it
    /// registered, whatever was granted (so its bookkeeping settles each frame).
    virtual double record(SceneFrame& frame, int kind, std::span<const WorldJob> granted, WorldStream& stream) = 0;
};

class SceneWorldService {
public:
    SceneWorldService();

    WorldService& policy() { return policy_; }
    const WorldService& policy() const { return policy_; }

    /// Register a kind of job `producer` makes (at most SceneGpuTimer::
    /// kMaxSpans); its index, -1 when there is no room.
    int addKind(WorldProducer* producer, const WorldJobKind& kind);

    /// The render begins: the renders `timer` read since the last call go to
    /// the policy, then its frame begins with `targetMs` as the target.
    /// `cut`: the view jumped this render, so the slack the renders before it
    /// left is forgotten and the new view settles (WorldService::cut), and those still to be
    /// read count for the kinds' costs only.
    void beginFrame(SceneGpuTimer* timer, float targetMs, bool cut = false);
    /// The view jumped between two renders: the view turned by more than half
    /// its vertical field of view (what lay at the centre of the screen,
    /// however far, has gone past its edge), or the eye moved more than
    /// kCutMetres, past what a camera travels in a frame. A wrong cut costs
    /// a frame or two at the floor; a missed one, a frame over the target.
    static constexpr float kCutMetres = 10.0f;
    static bool viewCut(const bromath::Vec3& eye, const bromath::Vec3& forward, const bromath::Vec3& wasEye,
                        const bromath::Vec3& wasForward, float halfFov);
    /// Kind `kind`'s granted jobs, recorded by `record` into the stream the
    /// kind's schedule names and stamped as the kind's span; the units it
    /// recorded.
    double run(WorldStreams& streams, int kind, const std::function<double(WorldStream&)>& record);

    /// A whole frame for the registered producers: beginFrame, their offers,
    /// the policy's grants, each kind run; the frame's stats.
    void runFrame(SceneFrame& frame);

private:
    WorldService policy_;
    std::vector<WorldProducer*> producers_;   // by kind
    std::array<std::string, WorldService::kMaxKinds> markNames_;   // "world:<kind>", a timed pass's name
    std::array<std::string, WorldService::kMaxKinds> offerNames_;  // "world offer:<kind>", a slow render's part
    uint64_t staleThrough_ = 0;   // renders up to this order were of a view before the last cut
    bool haveView_ = false;
    bromath::Vec3 eye_{0.0f, 0.0f, 0.0f}, forward_{0.0f, 0.0f, -1.0f};   // the last render's
};

}  // namespace bro::scene::vk
