#pragma once

#include "scene/scene_node.h"
#include <bromath/aabb.h>
#include <bromath/color.h>
#include <glad/gl.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace bro::scene {

/// World-space 3D particle emitter node. CPU-simulated in onTick() (a
/// fixed-size pool, no per-particle allocation after setMaxParticles) and
/// rendered as camera-facing instanced billboard quads in one draw call per
/// system, into the HDR scene FBO — depth-tested against geometry, not
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
    ~Particles3DNode() override;

    Particles3DNode(const Particles3DNode&) = delete;
    Particles3DNode& operator=(const Particles3DNode&) = delete;

    Type type() const override { return Type::Particles3D; }
    void onTick(float dtSec) override;

    // --- Capacity / texture / blend ---

    /// Hard cap on simultaneously alive particles. Excess emissions drop
    /// silently. Reallocates the pool — call once at setup.
    void setMaxParticles(int n);
    int  maxParticles() const { return static_cast<int>(particles_.size()); }

    /// Optional texture path (decoded lazily, uploaded on the GL thread at
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

    bool isPlaying() const { return playing_; }
    int  liveCount() const { return liveCount_; }

    // --- Bounds ---

    /// Conservative world-space AABB of live particles, padded by the max
    /// particle radius. Returns false when nothing is alive. Local-space
    /// sims transform the sim-space box corners by worldMatrix(). Consumed
    /// by frustum culling.
    bool worldBounds(bromath::AABB3& out) const;

    // --- GL (main GL thread only) ---

    /// Decode + upload the texture if a path is set. Returns the GL texture
    /// id (0 = none / decode failed — draw as soft round points).
    GLuint ensureTextureGL();

    /// Fill the per-instance buffer (back-to-front sorted for Normal blend),
    /// upload, and issue one instanced draw. `quadVbo` is the shared unit
    /// quad owned by the SceneRenderer; camera basis comes from the caller's
    /// bound program uniforms. Returns false if nothing drew.
    bool drawInstanced(GLuint quadVbo, const bromath::Vec3& camFwd);

    void releaseGL();

private:
    struct Particle {
        bool alive = false;
        bromath::Vec3 pos;   // sim-space (world or emitter-local)
        bromath::Vec3 vel;
        float life = 0;      // remaining seconds
        float maxLife = 0;
        float rot = 0;       // radians
        float spin = 0;      // radians/sec
    };

    void emitOne();
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

    std::function<void()> onFinished_;

    // Deterministic RNG stream (bromath splitmix64)
    uint64_t rng_ = 0x9e3779b97f4a7c15ull;

    // Sim-space bounds of live particles (exact per-tick min/max, expanded
    // on spawn), un-padded; worldBounds() pads and transforms.
    bromath::AABB3 bounds_;
    bool boundsValid_ = false;

    // Flipbook
    int sheetCols_ = 1, sheetRows_ = 1, sheetFrames_ = 1;

    // Texture (decoded lazily; GL upload on first draw)
    std::string texPath_;
    GLuint tex_ = 0;
    bool texTried_ = false;

    // Draw scratch + GL instance buffer
    static constexpr int kInstFloats = 10; // pos(3) size(1) rgba(4) rot(1) frame(1)
    std::vector<float> instanceData_;
    std::vector<uint32_t> drawOrder_;
    std::vector<float> depthKey_;
    GLuint vao_ = 0;
    GLuint instVbo_ = 0;
    size_t instVboCapacity_ = 0; // bytes
};

} // namespace bro::scene
