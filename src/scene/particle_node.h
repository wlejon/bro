#pragma once

#include "scene/scene_node.h"
#include "scene/shape_node.h"   // Color
#include <cstdint>
#include <string>
#include <vector>

namespace bro::scene {

/// 2D particle emitter node. Owns a fixed-size particle pool (no per-particle
/// allocations after the initial reserve) and integrates particles in
/// onTick(). Renders via the same CanvasScene path as ShapeNode/SpriteNode.
///
/// Position of the emitter is the SceneNode position; child-of-camera
/// integration is automatic via the standard 2D camera transform that
/// SceneGraph::render() pushes before walking nodes.
class ParticleNode : public SceneNode {
public:
    enum class Blend : uint8_t { Normal, Additive };

    explicit ParticleNode(const std::string& name = "");

    Type type() const override { return Type::Particles; }
    void onRender(SceneGraph& graph) override;
    void onTick(float dtSec) override;

    // --- Capacity / texture ---

    /// Hard cap on simultaneously alive particles. Excess emit() calls drop
    /// silently. Reallocates the pool — call once at setup.
    void setMaxParticles(int n);
    int  maxParticles() const { return static_cast<int>(particles_.size()); }

    /// Optional texture path (loaded lazily). When unset, particles render as
    /// filled circles using the start/end colours.
    void setTexturePath(const std::string& path);
    const std::string& texturePath() const { return texPath_; }

    void setBlend(Blend b) { blend_ = b; }
    Blend blend() const { return blend_; }

    // --- Emission ---

    void setRate(float perSec) { rate_ = perSec; }
    float rate() const { return rate_; }

    void setLifetime(float minSec, float maxSec) { lifeMin_ = minSec; lifeMax_ = maxSec; }
    void setVelocity(float angleDeg, float angleSpreadDeg, float speed, float speedSpread) {
        angle_ = angleDeg; angleSpread_ = angleSpreadDeg;
        speed_ = speed;    speedSpread_ = speedSpread;
    }
    void setGravity(float gx, float gy) { gravX_ = gx; gravY_ = gy; }
    void setSize(float startSize, float endSize) { sizeStart_ = startSize; sizeEnd_ = endSize; }
    void setColors(bromath::Color start, bromath::Color end) { colorStart_ = start; colorEnd_ = end; }
    void setRotation(float startDeg, float spinSpeedDeg, float spinSpreadDeg) {
        rotStart_ = startDeg; spinSpeed_ = spinSpeedDeg; spinSpread_ = spinSpreadDeg;
    }
    /// Drag is per-second multiplier applied as v *= drag (1.0 = none).
    void setDrag(float d) { drag_ = d; }

    // --- Control ---

    void play()  { playing_ = true; }
    /// Stop emitting; existing particles continue until their lifetime ends.
    void stop()  { playing_ = false; }
    /// Stop emitting and kill all live particles immediately.
    void clear();
    /// Emit `n` particles immediately, regardless of `rate`.
    void burst(int n);

    bool isPlaying() const { return playing_; }
    int  liveCount() const { return liveCount_; }

private:
    struct Particle {
        bool  alive = false;
        float x = 0, y = 0;
        float vx = 0, vy = 0;
        float life = 0;       // remaining seconds
        float maxLife = 0;    // total seconds
        float rot = 0;        // current rotation (radians)
        float spin = 0;       // angular velocity (radians/sec)
    };

    void emitOne();
    void ensureTextureLoaded();

    // Pool
    std::vector<Particle> particles_;
    int liveCount_ = 0;
    int searchHead_ = 0;
    bool playing_ = true;

    // Emission state
    float rate_ = 0.0f;
    float emitAccum_ = 0.0f;

    // Particle config
    float lifeMin_ = 0.5f, lifeMax_ = 1.0f;
    float angle_ = -90.0f, angleSpread_ = 360.0f;
    float speed_ = 100.0f, speedSpread_ = 50.0f;
    float gravX_ = 0.0f, gravY_ = 0.0f;
    float sizeStart_ = 6.0f, sizeEnd_ = 0.0f;
    bromath::Color colorStart_{1.0f, 1.0f, 1.0f, 1.0f};
    bromath::Color colorEnd_{1.0f, 1.0f, 1.0f, 0.0f};
    float rotStart_ = 0.0f;
    float spinSpeed_ = 0.0f;
    float spinSpread_ = 0.0f;
    float drag_ = 1.0f;
    Blend blend_ = Blend::Normal;

    // Optional texture
    std::string texPath_;
    std::vector<uint8_t> texPixels_;
    int texW_ = 0, texH_ = 0;
    bool texLoaded_ = false;
};

} // namespace bro::scene
