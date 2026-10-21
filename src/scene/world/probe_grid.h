#pragma once
// The probe grid (docs/scene-world-architecture.md §7, "Design: the probe
// grid"): irradiance probes about the eye, which light every lit surface's
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
// mod `columns` (a clipmap), so a move keeps every column still in the
// square where it was, and only the columns the move brought in are new.
//
// Volumes. Each cascade's ground heights (captured from above) and its
// canopy (the plants' crowns as a medium, and the sun's transmittance
// through it) are made for its corner: when it moves, when the canopy
// changes (canopy generation) or when the sun turns (sun generation), the
// cascade's volumes are a job, required (its probes can trace nothing
// without them), coarsest first: a finer cascade's sun rays leave its square
// through the coarser ones.
//
// Traces. A trace job is a box of a cascade's columns, every layer, its
// probes' rays traced and blended into what they held. The columns a move
// brought in are required, traced afresh (no history to blend); the rest are
// refreshed a slice of rows at a time, oldest first, their error growing with
// their age so the world service spends what the frame leaves on them.

#include <cstdint>
#include <vector>

namespace bro::scene {

struct ProbeGridConfig {
    int cascades = 4;
    int columns = 32;          // a cascade's columns a side
    int layers = 8;            // probes a column
    float spacing = 2.0f;      // cascade 0's column spacing (m)
    float ratio = 4.0f;        // each cascade's spacing over the last's
    int sliceRows = 4;         // a refresh's rows of columns
    // A refresh's error: this many pixels a second of its age in the finest
    // cascade, halved each coarser one (its probes light what is further
    // off, smaller on screen).
    float refreshPixelsPerSecond = 1.0f;
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

    /// The canopy (the plants' records) or the sun changed: every cascade's
    /// volumes are remade, then its probes refreshed.
    void setCanopyGeneration(uint64_t generation);
    void setSunGeneration(uint64_t generation);

    /// Whether cascade c's volumes are made for what it stands on now.
    bool volumesCurrent(int c) const;
    /// Its volumes made (the GPU recorded them this frame).
    void volumesMade(int c);
    /// Whether every cascade's volumes are made, so its probes can trace.
    bool traceable(int c) const { return volumesCurrent(c); }

    /// Seconds pass (the frame's step): the refresh slices' ages grow.
    void advance(float seconds);

    /// This frame's trace jobs: the fresh boxes (required) and every
    /// refresh slice (its error by age), of the cascades whose volumes are
    /// made. A box's index in this list is its job key.
    struct TraceJob {
        ProbeBox box;
        float error = 0.0f;
        bool required = false;
    };
    const std::vector<TraceJob>& traceJobs();
    /// Job `index` of the last traceJobs() was traced.
    void traced(int index);

private:
    struct Cascade {
        int64_t cornerX = 0, cornerZ = 0;
        bool placed = false;
        // What its volumes were made for.
        int64_t madeX = 0, madeZ = 0;
        uint64_t madeCanopy = 0, madeSun = 0;
        float madeTop = 0.0f;
        bool made = false;
        std::vector<ProbeBox> fresh;     // queued, required
        std::vector<float> sliceAge;     // seconds since each slice's refresh
    };
    void queueMove(Cascade& c, int index, int64_t dx, int64_t dz);

    ProbeGridConfig config_;
    std::vector<Cascade> cascades_;
    float top_ = 32.0f;
    uint64_t canopy_ = 1, sun_ = 1;
    std::vector<TraceJob> jobs_;
};

}  // namespace bro::scene
