#pragma once

#include <bromath/vec.h>
#include <bromath/quat.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace bro::scene {

class SceneGraph;

/// Engine-level property tween for scene nodes, Godot-Tween-flavored:
/// a sequence of steps, each step a group of property animations that run
/// together; steps run one after another, the whole sequence can loop, and
/// call-steps fire callbacks between them. Built by the JS bindings
/// (scene.createTween().to(...).call(...).start()), owned by the SceneGraph,
/// and ticked from SceneGraph::tickAnimations — the same clock as sprite and
/// skeletal animation, so it advances in the windowed frame loop and under
/// headless virtual time alike.
///
/// Property writes go through the same SceneNode/subclass setters the JS API
/// uses (setPosition/setRotation/setScale/setOpacity/setColor/...), so dirty
/// flags and GPU uploads happen exactly as if JS had set the property.
/// Targets are stored as node IDs and re-resolved through the graph on every
/// application — a destroyed node simply stops receiving writes.
///
/// Overshooting ticks carry across step boundaries (finish step N, remainder
/// flows into step N+1), so tween timing is independent of tick granularity.
class Tween {
public:
    enum class Ease : uint8_t {
        Linear,
        QuadIn, QuadOut, QuadInOut,
        CubicIn, CubicOut, CubicInOut,
        QuartIn, QuartOut, QuartInOut,
        QuintIn, QuintOut, QuintInOut,
        SineIn, SineOut, SineInOut,
        ExpoIn, ExpoOut, ExpoInOut,
        CircIn, CircOut, CircInOut,
        BackIn, BackOut, BackInOut,
        ElasticIn, ElasticOut, ElasticInOut,
        BounceIn, BounceOut, BounceInOut,
    };
    /// Parse "quadInOut" etc. Returns false (leaving `out` untouched) for an
    /// unknown name.
    static bool easeFromString(const std::string& name, Ease& out);
    static float applyEase(Ease e, float t);

    enum class Prop : uint8_t {
        Position,   // Vec3, node.setPosition
        Quaternion, // Quat slerp, node.setRotation
        Scale,      // Vec3, node.setScale
        Opacity,    // float — Sprite.setOpacity / Mesh color alpha
        Color,      // Vec3 rgb — Mesh/Light/Shape-fill color
        Custom,     // no node write; onUpdate(easedT) only
    };

    struct Anim {
        uint32_t nodeId = 0;              // 0 = no node (Custom)
        Prop prop = Prop::Custom;
        bromath::Vec3 v3To{};
        bromath::Quat qTo{};
        float fTo = 0.0f;
        float duration = 0.0f;            // seconds
        float delay = 0.0f;               // seconds before this anim starts
        Ease ease = Ease::Linear;
        std::function<void(float)> onUpdate; // eased t in [0,1] (may overshoot)

        // Captured when the anim first becomes active within its step.
        bool started = false;
        bromath::Vec3 v3From{};
        bromath::Quat qFrom{};
        float fFrom = 0.0f;
    };

    explicit Tween(uint32_t id) : id_(id) {}

    uint32_t id() const { return id_; }

    // --- Building (bindings) ---

    /// Append a group of anims as a new step — or merge into the previous
    /// anim step when parallel() armed it.
    void addAnims(std::vector<Anim> anims);
    /// Arm parallel merging: the next addAnims joins the previous anim step.
    void parallel() { parallelNext_ = true; }
    /// Append a zero-length callback step.
    void addCall(std::function<void()> fn);
    /// Loop the whole sequence `n` times; n < 0 = forever. Default 1.
    void setLoops(int n) { loops_ = n; }

    // --- Control ---

    void start();
    void stop();
    void pause()  { if (running_) paused_ = true; }
    void resume() { paused_ = false; }

    bool isRunning() const { return running_; }
    bool isPaused() const { return paused_; }
    bool isFinished() const { return finished_; }

    void setOnFinished(std::function<void()> cb) { onFinished_ = std::move(cb); }

    /// Deferred destruction: hides the tween from findTween immediately;
    /// SceneGraph erases it after the tick pass (a tween may destroy itself
    /// from its own callback).
    void markDestroyed() { destroyed_ = true; running_ = false; }
    bool destroyed() const { return destroyed_; }

    // --- Tick (SceneGraph::tickAnimations) ---

    void tick(float dtSec, SceneGraph& graph);

private:
    struct Step {
        std::vector<Anim> anims;
        std::function<void()> call; // set → call step (anims empty)
    };

    static float stepLength(const Step& s);
    void applyStep(Step& s, SceneGraph& graph);
    void resetProgress();
    void finish();

    uint32_t id_;
    std::vector<Step> steps_;
    bool parallelNext_ = false;

    int loops_ = 1;
    int loopsDone_ = 0;

    size_t stepIndex_ = 0;
    float stepElapsed_ = 0.0f;

    bool running_ = false;
    bool paused_ = false;
    bool finished_ = false;
    bool destroyed_ = false;
    uint64_t generation_ = 0; // bumped by start(); detects restart-from-callback

    std::function<void()> onFinished_;
};

} // namespace bro::scene
