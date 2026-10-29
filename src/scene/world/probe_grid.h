#pragma once
// The probe grid: irradiance probes about the eye, which light every lit surface's
// ambient. This is the CPU half, testable without a GPU: where the cascades
// lie, the heights of their probes, and the jobs they offer the world
// service. The GPU half (scene/vulkan/lighting/scene_probe_grid.h) owns the
// textures and records the jobs.
//
// Cascades. Cascade c is a square of `columns` x `columns` columns, spacing
// spacing * ratio^c metres apart, about the eye: its corner column (the
// first, in its own spacing) snaps so the eye stays in its middle column,
// and moves in whole columns. Column (i, j) stands at (i, j) * spacing in
// world metres, its probes at fixed heights above the ground there
// (layerHeight), the same in every cascade: a forest's light changes with
// height above its floor, whatever the distance.
//
// Storage wraps. A probe's place in its cascade's textures is its column
// mod `columns` (a clipmap), and so is its world's (the ground's heights,
// the canopy's voxels), so a move keeps every column still in the square
// where it was, and only the columns the move brought in are new.
//
// Volumes. Each cascade's ground heights (captured from above) and its
// canopy (the plants' crowns as a medium, and the sun's transmittance
// through it) are made a box of columns at a time: the strips a move
// brought in; the columns over a change of the canopy, and those its shade
// falls on; all of them when the sun turns or the layers' top moves. What
// is stale is a job, required (its probes can trace nothing without it).
//
// Traces. A trace job is a box of a cascade's columns, every layer, its
// probes' rays traced and blended into what they held. The columns a move
// brought in are traced afresh (no history to blend), in boxes of at most
// freshColumns columns (small enough for a frame's slack); the rest are
// refreshed a slice of rows at a time, each offered once its refresh period
// has passed, its error growing with its age so the oldest go first.
//
// Unseen columns. A lit surface blends a cascade into the next coarser over
// the outer columns of its square, and gives its outermost column no weight
// (scene_lighting.glsl's sceneProbe): the strip a one-column move brings
// in is not seen until the next move takes it inside. Its world and its
// fresh traces are wanted (unseenPixels, ahead of light that has only aged)
// but not required, so a walk's moves are made in the slack of the frames
// between them, not all in the frame of the move; until they are, the
// column holds what its storage held, the light of the column that left
// the square across from it. The last cascade, blended into nothing, is
// seen to its edge.

#include <cstdint>
#include <vector>

namespace bro::scene {

struct ProbeGridConfig {
    int cascades = 4;
    int columns = 32;          // a cascade's columns a side
    int layers = 8;            // probes a column
    float spacing = 2.0f;      // cascade 0's column spacing (m)
    float ratio = 4.0f;        // each cascade's spacing over the last's
    int sliceRows = 2;         // a refresh's rows of columns
    int freshColumns = 8;      // a fresh box's most columns
    float unseenPixels = 1.0f; // the error of an unseen column's world or fresh traces
    // A refresh's error: this many pixels a second of its age in the finest
    // cascade, halved each coarser one (its probes light what is further
    // off, smaller on screen). A slice just due is a quarter pixel wrong,
    // under what a shadow page still building is (half a pixel): light that
    // has probably not changed waits for what visibly has, until it has
    // waited a few periods.
    float refreshPixelsPerSecond = 0.25f;
    // A slice of the finest cascade is offered again this long after its
    // refresh, doubling each coarser cascade: the probes follow a turning
    // sun or a growing canopy over a few periods, and the frame's slack is
    // not all spent on retracing light that has not changed.
    float refreshSeconds = 1.0f;
};

/// A box of a cascade's columns, every layer: [x0, x1) x [z0, z1) in its
/// columns from the corner.
struct ProbeBox {
    int cascade = 0;
    int x0 = 0, z0 = 0, x1 = 0, z1 = 0;
    bool fresh = false;   // columns new to the square: no history
    int columnCount() const { return (x1 - x0) * (z1 - z0); }
};

class ProbeGrid {
public:
    void configure(const ProbeGridConfig& config);
    const ProbeGridConfig& config() const { return config_; }

    /// The top of the probes' layers above the ground (the canopy's top);
    /// a change rebuilds every cascade's volumes.
    void setTop(float metres);
    float top() const { return top_; }
    /// Layer k's height above the ground: the lowest just over it, the
    /// spacing growing upward (a canopy's light changes fastest near its
    /// floor); layerAt is its inverse, fractional, clamped to the layers.
    float layerHeight(int k) const;
    float layerAt(float height) const;
    static constexpr float kBottom = 0.25f;   // the lowest layer's height (m)
    static constexpr float kLayerPower = 1.6f;

    double spacing(int c) const;
    /// Cascade c's corner column (its first, in its spacing).
    int64_t cornerX(int c) const { return cascades_[static_cast<size_t>(c)].cornerX; }
    int64_t cornerZ(int c) const { return cascades_[static_cast<size_t>(c)].cornerZ; }

    /// The eye this frame (world metres): each cascade's corner snapped
    /// about it, and the columns a move brought in queued as fresh boxes.
    void place(double eyeX, double eyeZ);

    /// The canopy (the plants' records) changed over the ground box
    /// (x0, z0)-(x1, z1): the columns of each cascade over it, and those
    /// its shade falls on, are remade, then their probes refreshed. Without
    /// a box, everywhere.
    void canopyChanged(double x0, double z0, double x1, double z1);
    void canopyChanged();
    /// The sun (toward it), and its generation: a new one remakes every
    /// cascade's volumes.
    void setSun(float x, float y, float z, uint64_t generation);

    /// Whether a lit surface sees any of `box` (not all of it in its
    /// cascade's outermost columns, or the last cascade's).
    bool seen(const ProbeBox& box) const;

    /// Whether cascade c's volumes are made for what it stands on now.
    bool volumesCurrent(int c) const;
    /// Whether any of cascade c's stale volumes is seen.
    bool staleSeen(int c) const;
    /// The boxes of cascade c's columns whose volumes are stale (none when
    /// current; the whole square when nothing of it can be kept).
    void staleBoxes(int c, std::vector<ProbeBox>& out) const;
    /// Its volumes made (the GPU recorded them this frame).
    void volumesMade(int c);
    /// A cascade's stale boxes past this many are merged into the box
    /// about them (a frame's remake is a dispatch per box and stage).
    static constexpr int kMaxStaleBoxes = 8;

    /// Seconds pass (the frame's step): the refresh slices' ages grow.
    void advance(float seconds);

    /// This frame's trace jobs: the fresh boxes (required where seen; an
    /// unseen one once its world is made) and every refresh slice (its
    /// error by age), of every cascade, stale or not: what is stale and
    /// seen of its world is a required job of the frame's that records
    /// ahead of the traces. A box's index in this list is its job key.
    struct TraceJob {
        ProbeBox box;
        float error = 0.0f;
        bool required = false;
    };
    const std::vector<TraceJob>& traceJobs();
    /// The last traceJobs(), as it was.
    const std::vector<TraceJob>& offeredTraceJobs() const { return jobs_; }
    /// Job `index` of the last traceJobs() was traced.
    void traced(int index);

private:
    struct Cascade {
        int64_t cornerX = 0, cornerZ = 0;
        bool placed = false;
        // What its volumes were made for, and what of them is stale since.
        uint64_t madeSun = 0;
        float madeTop = 0.0f;
        bool made = false;   // false: all of it is stale
        std::vector<ProbeBox> stale;
        std::vector<ProbeBox> fresh;     // queued, required
        std::vector<float> sliceAge;     // seconds since each slice's refresh
    };
    void queueMove(Cascade& c, int index, int64_t dx, int64_t dz);
    /// `box` queued fresh, cut into boxes of at most freshColumns columns.
    void queueFresh(Cascade& c, const ProbeBox& box) const;
    /// Whether c's volumes over `box` are made.
    bool madeOver(const Cascade& c, const ProbeBox& box) const;
    /// `box` stale in c: merged with those it overlaps, and with all the
    /// rest past kMaxStaleBoxes.
    static void addStale(Cascade& c, ProbeBox box);

    ProbeGridConfig config_;
    std::vector<Cascade> cascades_;
    float top_ = 32.0f;
    uint64_t sun_ = 1;
    float sunDir_[3] = {0.0f, 1.0f, 0.0f};
    std::vector<TraceJob> jobs_;
};

}  // namespace bro::scene
