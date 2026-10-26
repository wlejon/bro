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
// of one unit's cost (observe()). The kind's cost is not their mean but
// leans to the dear ones: a sample moves it toward itself by a power of
// their ratio, costRate times costQuantile when the sample is dearer and
// times the rest when cheaper, so it settles where the dear samples' log
// ratios, so weighted, balance the cheap ones'. A kind whose jobs vary is
// priced near its dear end, and a frame's grants of it fit the budget when
// they come in dear; one whose jobs cost alike is priced at what they cost.
// (At the mean, a frame was granted fourteen of biomes' plant pages at
// 0.018 ms that took several times that each.)
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
// the order of the offers. Each kind's first job so ranked goes ahead of the
// rest, though: a kind whose jobs never run out (a moving sun's shadow pages,
// hundreds of them at 64 pixels' error a frame) would else take every frame
// at the floor, and a kind whose jobs are all less wrong would never be
// granted one (the land's tiles made stale under its regions waited behind
// them for good). A job is granted while the budget has room for its cost;
// one that does not fit is left, while a cheaper one may still fit. A kind
// granted nothing for starveFrames frames running has its first job granted
// whatever is left of the budget, if it costs no more than four times the
// budget (one job: a kind priced near the floor, or behind a carry that
// never clears, still moves, and its cost is measured again); while it is
// not, its cost falls by starveDecay a frame until it is. A kind priced at
// its dear end from a few slow spans (a frame the clouds' bake or a pipeline
// compile shared) was else never granted another to learn it cheaper: a
// land tile's fill alone, its world's whole module run over it, is some
// 0.1 to 0.2 ms on a 4090, and once priced past four budgets the tiles made
// stale under the regions stayed so for good, terraces of a coarser field's
// heights standing among the new.
//
// Budget. The executor reports each frame the GPU timer read (observe()):
// its GPU milliseconds and the service's own share of them. What the frame
// spent apart from the service, the largest over the last `window` frames
// read, is what the frame needs; the budget is what that leaves under the
// target less a headroom, never less than the floor. The headroom is the
// frames' surprise (a frame's own work and the jobs' costs both vary from
// what was expected of them): it rises a step on each frame read over the
// target and falls overRate / (1 - overRate) of a step on each under it, so
// it settles where overRate of the frames run over. A frame read over the
// target takes the budget to the floor until one comes in under it. Before any frame is read
// (or on a device that cannot time its frames) the budget is the floor.
//
// Catch-up. A kind may be a catch-up kind (WorldJobKind::catchUp: the
// land's tiles and regional grids, whose work missing is the ground itself
// wrong under the eye, and the plants placed on it waiting). While a frame
// leaves such a kind's work worse than catchUpError queued, the next frames
// fill to a ceiling of catchUpRatio times the target instead of the target
// (over that ceiling, the floor), and their frames over the target do not
// move the headroom. Spent from the target's slack alone, a view whose own
// frame stood near the target (the planet's ground, 2.2 ms of 2.78) had the
// floor every frame: a tile and two of a region's hundreds of steps a frame,
// its 55 regions and their tiles some 18,000 frames in coming. While behind,
// the need is the largest over the last catchUpWindow frames rather than
// `window`: the frame's own spikes (a plant page placed, a pipeline
// compiled) come every few dozen frames, between the short window's, and
// the ceiling stacked on one ran a frame to 8.5 ms. Catching up never
// spends less than keeping up would (the target less the short window's
// need): where the spikes reach most of the ceiling, the long window alone
// gave less than that, and a view's regions came slower behind than in
// step (the planet's region view, 0.5 ms a frame for 1.55). A catch-up kind whose
// steps cost unlike (a region's init bands, its passes, its finish) is
// several kinds, each priced by its own spans, so a grant is what it spends.
//
// A cut (cut(): the view jumped) forgets the slack, since a frame's cost is
// its view's and the old view's says nothing of the new one's, and lets the
// new view settle: its budget is at least settleMs until it has left
// nothing worse than settleError queued for settleQuiet frames running, or
// settleFrames have passed. A cut shows a new place whose world is all
// wanted at once (biomes' glade from the meadow: 1,655 jobs, its turf, its
// still and plants shadow pages, three land's sun levels), and the frame
// after it is a hitch whatever is spent; on the running budget the new
// place took 64 frames to settle, the shade of the place left behind on it
// for 20 of them, and a still taken a second in showed a straight-edged lit
// field where, settled, the trees shade it. At 8 ms it settles in five
// frames.
// A compute span on its own queue runs beside the frame, so only the
// graphics' spans are taken off the frame's time.
//
// A kind drawn the next frame (WorldJobKind::nextFrame: the shadow pages,
// whose frame plans its pages before the service runs) spends the budget of
// the frame that draws it. A frame's grants of such kinds are carried into
// the next one, whose jobs drawn at once fit what the carry leaves of its
// budget; its grants drawn the next frame fit the budget less its own jobs
// drawn at once (the next frame's, as far as it can tell). A job drawn at
// once that finds the carry in its way holds its cost back from them and
// from the jobs drawn at once ranked under it, so the next frame has room
// for it: a kind drawn later is never starved by one drawn sooner, nor the
// reverse, nor a dear job by the cheap ones under it that would take the
// slack it waits for. (Charged to the frame
// that grants them, the next frame's grants came on top of its own budget:
// a frame drew the pages granted it last frame and spent its budget again.)

#include <algorithm>
#include <atomic>
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
    bool nextFrame = false;   // its grants are drawn the next frame (see Budget)
    bool catchUp = false;     // its work queued past catchUpError lifts the frame's ceiling (see Catch-up)
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
    float floorShare = 0.1f;  // and of the frame's own need over the short window, while over the target
    int window = 4;           // frames whose need sets the slack
    float costRate = 0.25f;      // how far one measurement moves a kind's cost (the power of its ratio)
    float costQuantile = 0.9f;   // the weight of a dearer measurement's pull, the rest a cheaper one's
    float overRate = 0.005f;   // the share of frames the headroom lets run over the target
    float headroomStep = 0.01f;   // how far one frame read moves the headroom (ms)
    float settleMs = 8.0f;      // the least budget a frame while a cut settles
    float settleError = 1.0f;   // a cut has settled when nothing worse than this (pixels) is left queued
    int settleQuiet = 4;        // for this many frames running (what the frames in flight read back)
    int settleFrames = 30;      // and has settled, whatever is left, after this many frames
    float catchUpRatio = 1.4f;   // while a catch-up kind is behind, the frame's ceiling is the target times this
    float catchUpError = 2.0f;   // a catch-up kind is behind while it left work worse than this (pixels) queued
    int catchUpWindow = 120;     // frames whose need sets the slack while behind (see Catch-up)
    int starveFrames = 2;     // a kind granted nothing this many frames running has its first job granted
    float starveDecay = 0.8f; // and its cost times this each frame more it is not (until one is)
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
    float carriedMs = 0.0f;   // last frame's grants of kinds drawn the next frame, drawn in this one
    uint32_t offered = 0;
    uint32_t granted = 0;
    uint32_t queued = 0;      // offered and left for a later frame
    float worstError = 0.0f;  // the largest error left queued, pixels (0: none)
    bool catchingUp = false;  // a catch-up kind was left behind: the next frame spends under the catch-up ceiling
};

class WorldService {
public:
    static constexpr int kMaxKinds = 16;

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
    /// Kind `k`'s jobs this frame left queued, and the largest error among them.
    uint32_t queued(int k) const { return kinds_[static_cast<size_t>(k)].queued; }
    float worstError(int k) const { return kinds_[static_cast<size_t>(k)].worstError; }

    /// A frame the GPU timer read (oldest first, each once).
    void observe(const WorldFrameTiming& timing);
    /// The view jumped: what the last frames left was measured in a view no
    /// longer shown, so it is forgotten, and the new view settles (its
    /// budget at least settleMs until it has). (Kinds keep their costs.)
    void cut() {
        need_.clear();
        over_ = false;
        settle_ = config_.settleFrames;
        quiet_ = 0;
    }
    bool settling() const { return settle_ > 0; }
    /// Every service spends at least settleMs a frame while on, as a cut
    /// that settles does: a headless wait for the scene to settle
    /// (perf.settle) runs frames for their results, not to a frame time.
    static void settleAll(bool on) { s_settlingAll.store(on, std::memory_order_relaxed); }
    /// A catch-up kind left work worse than catchUpError queued last frame,
    /// so this frame's budget is spent under the catch-up ceiling.
    bool catchingUp() const { return behind_; }
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
    static inline std::atomic<bool> s_settlingAll{false};
    /// The frame's GPU ms the budget fills to: the target, or the catch-up
    /// ceiling while behind.
    float ceiling() const {
        return behind_ ? std::max(config_.targetMs, config_.targetMs * config_.catchUpRatio) : config_.targetMs;
    }
    struct Kind {
        WorldJobKind desc;
        float cost = 0.05f;
        bool measured = false;
        int starved = 0;   // frames running it offered jobs and was granted none
        uint32_t queued = 0;      // this frame's jobs left for a later one
        float worstError = 0.0f;  // and the largest error among them (pixels)
    };
    std::vector<Kind> kinds_;
    WorldServiceConfig config_;
    std::vector<double> need_;   // the last frames' ms apart from the service, newest last
    bool over_ = false;          // the last frame read ran over the target (the catch-up ceiling while behind)
    bool overTarget_ = false;    // the last frame read ran over the target
    bool behind_ = false;        // a catch-up kind left work worse than catchUpError queued last frame
    int settle_ = 0;             // frames a cut has left to settle in
    int quiet_ = 0;              // frames running it has left nothing worse than settleError queued
    float headroom_ = 0.0f;      // kept under the target (ms): the frames' surprise at overRate
    float budget_ = 0.0f;
    float carried_ = 0.0f;       // the last frame's grants of kinds drawn the next frame (ms)
    std::vector<WorldJob> offers_;
    std::vector<std::vector<WorldJob>> granted_;
    WorldServiceStats stats_;
};

}  // namespace bro::scene
