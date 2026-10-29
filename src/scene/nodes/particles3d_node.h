#pragma once

#include "scene/scene_node.h"
#include <bromath/aabb.h>
#include <bromath/color.h>
#include "scene/texture_source.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace bro::scene {

/// World-space 3D particle emitter node. CPU-simulated in onTick() (a
/// fixed-size pool, no per-particle allocation after setMaxParticles) and
/// rendered as camera-facing instanced billboard quads in one draw call per
/// system, into the HDR scene target — depth-tested against geometry, not
/// depth-writing, before tonemap so additive systems bloom.
///
/// Simulation space: World keeps particles where they were spawned (a moving
/// emitter leaves a trail); Local integrates in emitter space so the whole
/// cloud rides the node transform.
///
/// Deterministic: all randomness comes from a splitmix64 stream seeded by
/// setSeed(), so a fixed seed + fixed dt steps reproduce exactly.
class Particles3DNode : public SceneNode {
public:
    enum class Blend : uint8_t { Normal, Additive };
    enum class EmitterShape : uint8_t { Point, Sphere, Hemisphere, Box, Cone };
    enum class SimSpace : uint8_t { World, Local };

    explicit Particles3DNode(const std::string& name = "");
    ~Particles3DNode() override = default;

    Particles3DNode(const Particles3DNode&) = delete;
    Particles3DNode& operator=(const Particles3DNode&) = delete;

    Type type() const override { return Type::Particles3D; }
    void onTick(float dtSec) override;

    // --- Capacity / texture / blend ---

    /// Hard cap on simultaneously alive particles. Excess emissions drop
    /// silently. Reallocates the pool — call once at setup.
    void setMaxParticles(int n);
    int  maxParticles() const { return static_cast<int>(particles_.size()); }

    /// Optional texture path (decoded lazily, uploaded by the renderer at
    /// first draw). When unset, particles render as soft round points.
    void setTexturePath(const std::string& path);
    const std::string& texturePath() const { return texPath_; }

    /// Flipbook sprite-sheet grid on the texture: cols x rows cells played
    /// over each particle's lifetime. `frames` limits playback to the first
    /// N cells (0 = cols*rows). (1,1) disables the flipbook.
    void setSheet(int cols, int rows, int frames = 0);
    int sheetCols() const { return sheetCols_; }
    int sheetRows() const { return sheetRows_; }

    void setBlend(Blend b) { blend_ = b; }
    Blend blend() const { return blend_; }

    /// Soft-particle fade distance in world units: fragments fade in over
    /// this depth gap to the opaque scene behind them, removing the hard
    /// clip line where quads intersect geometry. 0 (default) = off; the
    /// renderer only pays for the per-frame depth snapshot while at least
    /// one live system requests softness.
    void setSoftness(float d) { softness_ = d < 0.0f ? 0.0f : d; }
    float softness() const { return softness_; }

    // --- Emitter shape / space ---

    void setShape(EmitterShape s) { shape_ = s; }
    EmitterShape shape() const { return shape_; }

    /// Sphere/hemisphere/cone radius (world units).
    void setShapeRadius(float r) { shapeRadius_ = r < 0.0f ? 0.0f : r; }
    float shapeRadius() const { return shapeRadius_; }

    /// Box half-extents.
    void setShapeExtents(const bromath::Vec3& he) { shapeExtents_ = he; }
    const bromath::Vec3& shapeExtents() const { return shapeExtents_; }

    /// Cone half-angle in degrees (spread of the cone from its axis).
    void setConeAngle(float deg) { coneAngleDeg_ = deg; }
    float coneAngle() const { return coneAngleDeg_; }

    void setSpace(SimSpace s) { space_ = s; }
    SimSpace space() const { return space_; }

    // --- Emission ---

    void setRate(float perSec) { rate_ = perSec; }
    float rate() const { return rate_; }

    void setLifetime(float minSec, float maxSec) { lifeMin_ = minSec; lifeMax_ = maxSec; }

    /// Launch direction (emitter-local; normalized internally) and cone full
    /// width in degrees around it. Sphere/hemisphere shapes launch radially
    /// instead and only apply the spread jitter.
    void setDirection(const bromath::Vec3& dir, float spreadDeg);
    void setSpeed(float speed, float spread) { speed_ = speed; speedSpread_ = spread; }

    void setGravity(const bromath::Vec3& g) { gravity_ = g; }
    /// Drag is a per-second velocity multiplier (1.0 = none).
    void setDrag(float d) { drag_ = d; }

    void setSize(float startSize, float endSize) { sizeStart_ = startSize; sizeEnd_ = endSize; }
    void setColors(bromath::Color start, bromath::Color end) {
        colorStops_ = {{0.0f, start}, {1.0f, end}};
    }
    /// Gradient stops over normalized life [0,1]. Must be sorted by t.
    void setColorStops(std::vector<std::pair<float, bromath::Color>> stops);

    void setRotation(float startDeg, float spinSpeedDeg, float spinSpreadDeg) {
        rotStartDeg_ = startDeg; spinSpeedDeg_ = spinSpeedDeg; spinSpreadDeg_ = spinSpreadDeg;
    }

    /// One-shot / looping emission window. duration <= 0 = continuous.
    /// With duration > 0: loop restarts the window each cycle; otherwise the
    /// system emits for `duration` seconds, drains, and fires onFinished once.
    void setDuration(float seconds, bool loop) { duration_ = seconds; loop_ = loop; }
    float duration() const { return duration_; }
    bool loops() const { return loop_; }

    /// Reseed the deterministic RNG stream.
    void setSeed(uint64_t seed) { rng_ = seed ? seed : 0x9e3779b97f4a7c15ull; }

    /// One-shot completion callback. Fired by SceneGraph::tickAnimations
    /// after this node's onTick returns (never from inside it), so the
    /// callback may destroy the node.
    void setOnFinished(std::function<void()> cb) { onFinished_ = std::move(cb); }
    bool finishedPending() const { return finishedPending_; }
    std::function<void()> consumeFinishedCallback() {
        finishedPending_ = false;
        return onFinished_;
    }

    // --- Control ---

    void play();
    /// Stop emitting; existing particles continue until their lifetime ends.
    void stop() { playing_ = false; }
    /// Stop emitting and kill all live particles immediately.
    void clear();
    /// Emit `n` particles immediately, regardless of `rate`.
    void burst(int n);

    /// Per-call overrides for emit(): one pooled system serves many
    /// one-shot effects (every impact, muzzle puff or explosion of a kind)
    /// without reconfiguring the shared emitter. `position` is in sim space
    /// (world for World systems) and replaces the node transform; the
    /// emitter shape is sampled around it unrotated. `direction` and
    /// `spreadDeg` replace the launch cone; `speed`/`speedSpread` replace
    /// the launch speed. `sizeScale` and `lifeScale` multiply the system's
    /// size curve and lifetime for these particles only, and `tint`
    /// multiplies their colour over life (rgb may exceed 1 to push additive
    /// systems past the bloom threshold).
    struct EmitOverride {
        bromath::Vec3 position{0.0f, 0.0f, 0.0f};
        bromath::Vec3 direction{0.0f, 1.0f, 0.0f};
        float spreadDeg = 0.0f;
        float speed = 1.0f;
        float speedSpread = 0.0f;
        float sizeScale = 1.0f;
        float lifeScale = 1.0f;
        bromath::Color tint{1.0f, 1.0f, 1.0f, 1.0f};
    };
    void emit(int n, const EmitOverride& o);

    /// Freeze the simulation: particles hold their age, place and colour
    /// and no rate emission happens until resumed. emit()/burst() still add.
    void setPaused(bool p) { paused_ = p; }
    bool paused() const { return paused_; }

    bool isPlaying() const { return playing_; }
    int  liveCount() const { return liveCount_; }

    // --- Bounds ---

    /// Conservative world-space AABB of live particles, padded by the max
    /// particle radius. Returns false when nothing is alive. Local-space
    /// sims transform the sim-space box corners by worldMatrix(). Consumed
    /// by frustum culling.
    bool worldBounds(bromath::AABB3& out) const;

    /// Build back-to-front sorted instance data for rendering.
    /// Format: pos(3) size(1) rgba(4) rot(1) frame(1) = 10 floats per instance.
    const std::vector<float>& buildInstanceData(const bromath::Vec3& camFwd);
    size_t activeParticleCount() const { return drawOrder_.size(); }
    bool hasTexture() const { return !texPath_.empty(); }
    /// Decode the setTexturePath() file once; false when there is no path or
    /// it failed to decode (the particles then draw as soft round points).
    bool ensureTextureLoaded();
    const NodeTexture& texture() const { return texture_; }

private:
    struct Particle {
        bool alive = false;
        bromath::Vec3 pos;   // sim-space (world or emitter-local)
        bromath::Vec3 vel;
        float life = 0;      // remaining seconds
        float maxLife = 0;
        float rot = 0;       // radians
        float spin = 0;      // radians/sec
        float sizeScale = 1; // emit() override, 1 for plain emission
        bromath::Color tint{1.0f, 1.0f, 1.0f, 1.0f};
    };

    void emitOne(const EmitOverride* o = nullptr);
    /// Sample a spawn position + launch direction in emitter-local space.
    void sampleEmitter(bromath::Vec3& outPos, bromath::Vec3& outDir);
    bromath::Color evalColor(float u) const;
    bool emissionActive() const {
        return playing_ && (duration_ <= 0.0f || emitClock_ < duration_);
    }

    // Pool
    std::vector<Particle> particles_;
    int liveCount_ = 0;
    int searchHead_ = 0;
    bool playing_ = true;

    // Emission state
    float rate_ = 0.0f;
    float emitAccum_ = 0.0f;
    float emitClock_ = 0.0f;    // seconds since play() (duration window)
    bool finishedPending_ = false;
    bool finishedFired_ = false;

    // Emitter config
    EmitterShape shape_ = EmitterShape::Point;
    SimSpace space_ = SimSpace::World;
    float shapeRadius_ = 0.5f;
    bromath::Vec3 shapeExtents_{0.5f, 0.5f, 0.5f};
    float coneAngleDeg_ = 25.0f;
    bromath::Vec3 direction_{0.0f, 1.0f, 0.0f};
    float spreadDeg_ = 0.0f;

    // Particle config
    float lifeMin_ = 0.5f, lifeMax_ = 1.0f;
    float speed_ = 1.0f, speedSpread_ = 0.0f;
    bromath::Vec3 gravity_{0.0f, 0.0f, 0.0f};
    float drag_ = 1.0f;
    float sizeStart_ = 0.1f, sizeEnd_ = 0.0f;
    std::vector<std::pair<float, bromath::Color>> colorStops_ = {
        {0.0f, {1.0f, 1.0f, 1.0f, 1.0f}},
        {1.0f, {1.0f, 1.0f, 1.0f, 0.0f}},
    };
    float rotStartDeg_ = 0.0f;
    float spinSpeedDeg_ = 0.0f;
    float spinSpreadDeg_ = 0.0f;
    float duration_ = 0.0f;
    bool loop_ = true;
    Blend blend_ = Blend::Normal;
    float softness_ = 0.0f;
    bool paused_ = false;
    float maxSizeScale_ = 1.0f; // largest live sizeScale, pads worldBounds

    std::function<void()> onFinished_;

    // Deterministic RNG stream (bromath splitmix64)
    uint64_t rng_ = 0x9e3779b97f4a7c15ull;

    // Sim-space bounds of live particles (exact per-tick min/max, expanded
    // on spawn), un-padded; worldBounds() pads and transforms.
    bromath::AABB3 bounds_;
    bool boundsValid_ = false;

    // Flipbook
    int sheetCols_ = 1, sheetRows_ = 1, sheetFrames_ = 1;

    // Texture (decoded lazily on the render path)
    std::string texPath_;
    NodeTexture texture_;
    bool texTried_ = false;

    // Draw scratch
    static constexpr int kInstFloats = 10; // pos(3) size(1) rgba(4) rot(1) frame(1)
    std::vector<float> instanceData_;
    std::vector<uint32_t> drawOrder_;
    std::vector<float> depthKey_;
};

} // namespace bro::scene
