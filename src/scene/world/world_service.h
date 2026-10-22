#pragma once
// The world service's policy (docs/scene-world-architecture.md §6): one queue
// of jobs across every producer of view-independent world work, ranked by
// what is most wrong on screen, spent from a budget in milliseconds that the
// frame's measured slack sets. This is the CPU half, testable without a GPU;
// the executor (scene/vulkan/world/scene_world_service.h) opens the streams
// the jobs record into and hands their GPU timestamps back here.
//
// Kinds. A producer registers each kind of job it makes (addKind): its name,
// its schedule (a raster job draws on the graphics queue at the frame's
// head; a compute job goes to the async compute queue where the device has
// one, else into the graphics stream at the same point) and a first guess of
// one unit's GPU milliseconds. A job's `units` are its work in its kind's
// measure (1 where a kind's jobs cost alike; a probe box's probe-rays, where
// they do not), and its cost is the kind's cost a unit times them. The guess
// stands until the kind's jobs are measured: each frame the executor times
// a kind's jobs as one span, and the span over the units it held is a sample
// of one unit's cost, which the kind's cost follows (observe()).
//
// Jobs. Each frame a producer offers what it would make (offer()): a job is
// keyed by the producer (`key`, `producer`: a cell key, a probe's place, a
// shadow page, and which of the producer's sets it belongs to), carries its
// error in pixels while it is not made (what is shown in its place: a
// missing tile's coarser parent, a stale tile's old contents) and its size,
// and may be `required` (the coarsest level of a producer, which is always
// resident: made whatever the budget). The producer keeps its own policy
// (which tiles it wants); the service only orders and grants.
//
// Ranking. Required jobs first; then by error, largest first, whoever the
// producer; ties to the larger size, so the world fills in from the top
// down; then by kind, producer and key, so a frame's order never depends on
// the order of the offers. A job is granted while the budget has room for
// its cost; one that does not fit is left, while a cheaper one may still fit.
//
// Budget. The executor reports each frame the GPU timer read (observe()):
// its GPU milliseconds and the service's own share of them. What the frame
// spent apart from the service, the largest over the last `window` frames
// read, is what the frame needs; the budget is what that leaves under the
// target, never less than the floor. A frame read over the target takes the
// budget to the floor until one comes in under it. Before any frame is read
// (or on a device that cannot time its frames) the budget is the floor, and
// so it is again after the view jumps (forgetSlack()), until a frame of the
// new view is read: a frame's cost is its view's, and the slack the old view
// left says nothing of the new one's.
// A compute span on its own queue runs beside the frame, so only the
// graphics' spans are taken off the frame's time.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace bro::scene {

enum class WorldSchedule : uint8_t {
    Raster,    // draws: on the graphics queue, at the frame's head
    Compute,   // dispatches: on the async compute queue, else at the frame's head
};

struct WorldJobKind {
    std::string name;
    WorldSchedule schedule = WorldSchedule::Raster;
    float costMs = 0.05f;   // one unit's GPU ms until measured
};

/// A job a producer offers this frame.
struct WorldJob {
    uint64_t key = 0;        // the producer's (a cell key's packed bits, ...)
    uint32_t producer = 0;   // which of the producer's sets (a grass node's id, ...)
    int kind = 0;
    float error = 0.0f;      // pixels on screen while it is not made
    double size = 0.0;       // what it covers (metres): ties go to the larger
    bool required = false;   // made whatever the budget
    double units = 1.0;      // its work in its kind's measure: its cost is the kind's a unit times this
};

struct WorldServiceConfig {
    float targetMs = 2.78f;   // the frame's GPU target (the 4090 at 360 fps)
    float floorMs = 0.1f;     // the least budget a frame
    int window = 4;           // frames whose need sets the slack
    float costRate = 0.25f;   // how far one measurement moves a kind's cost
};

/// A frame the GPU timer read, as the service sees it.
struct WorldFrameTiming {
    double frameMs = -1.0;   // the frame's GPU ms (-1: not timed)
    struct Span {
        int kind = 0;
        double units = 0.0;    // the work the span held (its jobs' units)
        double ms = 0.0;
        bool apart = false;    // on the async compute queue, beside the frame
    };
    std::vector<Span> spans;
};

struct WorldServiceStats {
    float budgetMs = 0.0f;    // this frame's
    float spentMs = 0.0f;     // the granted jobs' cost, by their kinds' measured costs
    uint32_t offered = 0;
    uint32_t granted = 0;
    uint32_t queued = 0;      // offered and left for a later frame
    float worstError = 0.0f;  // the largest error left queued, pixels (0: none)
};

class WorldService {
public:
    static constexpr int kMaxKinds = 8;

    void configure(const WorldServiceConfig& config) { config_ = config; }
    const WorldServiceConfig& config() const { return config_; }
    void setTarget(float ms) { config_.targetMs = ms; }

    /// Register a kind of job; its index (-1 past kMaxKinds).
    int addKind(const WorldJobKind& kind);
    int kindCount() const { return static_cast<int>(kinds_.size()); }
    const WorldJobKind& kind(int k) const { return kinds_[static_cast<size_t>(k)].desc; }
    /// One unit's GPU ms, as last measured (its guess before).
    float cost(int k) const { return kinds_[static_cast<size_t>(k)].cost; }
    bool measured(int k) const { return kinds_[static_cast<size_t>(k)].measured; }

    /// A frame the GPU timer read (oldest first, each once).
    void observe(const WorldFrameTiming& timing);
    /// The view jumped: what the last frames left was measured in a view no
    /// longer shown, so it is forgotten, and the budget is the floor until a
    /// frame of the new view is read. (Kinds keep their costs.)
    void forgetSlack() {
        need_.clear();
        over_ = false;
    }
    /// Start a frame: drop the last one's offers and set this one's budget.
    void beginFrame();
    void offer(const WorldJob& job) { offers_.push_back(job); }
    /// Rank the offers and grant what the budget holds.
    void schedule();
    /// Kind `k`'s granted jobs this frame, in rank order.
    std::span<const WorldJob> granted(int k) const;

    float budgetMs() const { return budget_; }
    const WorldServiceStats& stats() const { return stats_; }

private:
    struct Kind {
        WorldJobKind desc;
        float cost = 0.05f;
        bool measured = false;
    };
    std::vector<Kind> kinds_;
    WorldServiceConfig config_;
    std::vector<double> need_;   // the last frames' ms apart from the service, newest last
    bool over_ = false;          // the last frame read ran over the target
    float budget_ = 0.0f;
    std::vector<WorldJob> offers_;
    std::vector<std::vector<WorldJob>> granted_;
    WorldServiceStats stats_;
};

}  // namespace bro::scene
