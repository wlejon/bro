#pragma once

#include "scene/tween.h"  // Tween::Ease — the shared named-easing table

#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace bro::scene {

class SceneGraph;
class SceneNode;

/// Property targets a clip track can animate. Position/Rotation/Scale apply
/// to every node type; the rest are validated against the target node's type
/// at play() time (a clear error, not a silent no-op):
///   Opacity   — SpriteNode opacity / MeshNode color alpha
///   Color     — MeshNode color rgb / LightNode color / ShapeNode fill rgb
///   Fov       — CameraNode vertical FOV (radians)
///   Intensity — LightNode intensity
///   Range     — LightNode range
///   Emissive  — MeshNode emissive scalar
///   Metallic  — MeshNode metallic
///   Roughness — MeshNode roughness
enum class ClipProp : uint8_t {
    Position, Rotation, Scale, Opacity, Color,
    Fov, Intensity, Range, Emissive, Metallic, Roughness,
};

/// Per-key interpolation mode for the segment LEAVING the key:
///   Linear — lerp (Rotation: shortest-path slerp)
///   Step   — hold the key's value until the next key
///   Cubic  — Catmull-Rom: tangents derived from the neighboring keys
///            ((v[k+1]-v[k-1])/(t[k+1]-t[k-1]), one-sided at the endpoints),
///            evaluated with the same Hermite basis the rigging clips use for
///            glTF CUBICSPLINE (Rotation: component-wise with hemisphere
///            alignment, then normalize)
enum class ClipInterp : uint8_t { Linear, Step, Cubic };

/// Data-driven multi-track keyframe clip for arbitrary scene-node properties
/// — the Godot Animation-resource analog for bro's 3D scene. Built from a
/// plain-JSON clipDef by the JS bindings (player.addClip), immutable once
/// registered (replacing a name installs a new clip; an in-flight playback
/// keeps its shared_ptr to the old one). `sourceJson` carries the verbatim
/// clipDef for round-trip (player.clipDef(name)) so apps can persist clips
/// to files without a bespoke format.
struct AnimationClip {
    enum class Loop : uint8_t { None, Loop, PingPong };

    struct PropTrack {
        std::string target;              ///< node NAME, resolved at play()
        ClipProp prop = ClipProp::Position;
        int stride = 3;                  ///< floats per key: 1, 3, or 4 (quat)
        // Parallel per-key arrays, sorted ascending by time.
        std::vector<float> times;
        std::vector<float> values;       ///< stride * times.size()
        std::vector<ClipInterp> interps; ///< segment leaving key k
        std::vector<Tween::Ease> eases;  ///< u-warp on the segment leaving key k
    };

    struct EventKey {
        float time = 0.0f;
        std::string name;
        std::string argsJson;            ///< "" = no args; else JSON payload
    };
    struct EventTrack {
        std::vector<EventKey> keys;      ///< sorted ascending by time
    };

    float duration = 0.0f;
    Loop loop = Loop::None;
    std::vector<PropTrack> props;
    std::vector<EventTrack> events;
    std::string sourceJson;
};

/// Plays AnimationClips against a SceneGraph — the Godot AnimationPlayer
/// analog for node properties (the skeletal AnimationPlayer in
/// animation_player.h drives bone palettes and is untouched; a clip animating
/// a skinned node's TRS composes with it, different domains).
///
/// Created via SceneGraph::createClipPlayer() (JS:
/// scene.createAnimationPlayer()), owned by the graph, referenced by id, and
/// ticked from SceneGraph::tickAnimations AFTER node ticks and tweens — so
/// among systems writing the same property in the same frame the order is:
/// skeletal/sprite animation, then tweens, then clip players (each category
/// in creation order), last writer wins. Runs on the scaled clock, so
/// bro.time pause/scale and headless virtual time apply automatically.
///
/// Playback semantics:
///   - play(name, {speed, from, fade}) resolves every track's target node by
///     NAME (first match) and validates property/node-type compatibility —
///     both failures report a clear error instead of silently no-opping.
///     Targets are then tracked by node id: a node destroyed mid-playback
///     simply stops receiving writes (Tween's liveness discipline).
///   - speed < 0 plays in reverse; `from` defaults to 0 forward, duration in
///     reverse. speed is live-settable mid-playback.
///   - Loop::None holds the final value and fires onFinished once;
///     Loop::Loop wraps; Loop::PingPong reflects at both ends (a full cycle
///     is 2x duration).
///   - Event keys fire through onEvent exactly once when the playhead crosses
///     them (in either direction; reverse playback fires them in reverse
///     order). Looping re-fires them each pass. seek() does NOT retro-fire
///     skipped events (Godot semantics) and does not fire the key sitting
///     exactly at the seek position; play() DOES fire a key at exactly the
///     start position. Event callbacks run during the tick, before the
///     frame's property writes.
///   - seek(t) clamps to [0, duration], clears the finished latch, cancels
///     an in-progress crossfade, and evaluates + writes immediately (works
///     while paused — scrubbing).
///   - play(name, {fade: seconds}) crossfades: the outgoing clip keeps
///     advancing (property tracks only — its event tracks stop firing at the
///     switch) and each incoming track blends from the matching outgoing
///     track's value, or from the target's captured current value when the
///     outgoing clip does not animate that property. Outgoing tracks with no
///     incoming counterpart keep writing until the fade ends, then freeze.
///
/// Per-frame evaluation allocates nothing: targets and property accessors are
/// resolved at play() time, keys live in flat sorted arrays walked by a
/// cached cursor per track, and evaluation is a sample pass into fixed
/// per-track value slots followed by a write pass (kept separate so blend
/// weights can slot in between later).
class ClipPlayer {
public:
    explicit ClipPlayer(uint32_t id) : id_(id) {}

    uint32_t id() const { return id_; }

    // --- Setup ---

    /// Register a clip under `name`, replacing any existing one (an active
    /// playback of the old clip keeps its reference).
    void addClip(const std::string& name,
                 std::shared_ptr<const AnimationClip> clip);
    bool hasClip(const std::string& name) const {
        return clips_.find(name) != clips_.end();
    }
    /// The registered clip, or nullptr. Used by clipDef() round-trip.
    const AnimationClip* clip(const std::string& name) const;

    // --- Playback ---

    struct PlayOptions {
        float speed = 1.0f;
        float from = NAN;    ///< NAN → 0 forward / duration in reverse
        float fade = 0.0f;   ///< seconds of crossfade from the current state
    };

    /// Start a clip. On failure returns false with a human-readable reason in
    /// `err` (unknown clip, unresolvable target name, property/node-type
    /// mismatch) and leaves the current playback untouched.
    bool play(const std::string& name, const PlayOptions& opts,
              SceneGraph& graph, std::string& err);

    void pause()  { paused_ = true; }
    void resume() { paused_ = false; }
    /// Stop and forget the current clip (no more writes; currentClip reads
    /// "" and currentTime 0). Also drops any crossfade source.
    void stop();

    /// Scrub to `t` seconds (see class comment for exact semantics).
    void seek(float t, SceneGraph& graph);

    float speed() const { return current_.clip ? current_.speed : 1.0f; }
    void setSpeed(float s) { if (current_.clip) current_.speed = s; }

    float currentTime() const { return current_.clip ? current_.time : 0.0f; }
    const std::string& currentClip() const { return current_.name; }
    /// True while a clip is actively advancing (not paused, not finished).
    bool playing() const {
        return current_.clip != nullptr && !paused_ && !current_.finished;
    }

    /// Fired once when a Loop::None clip reaches its end (start, in
    /// reverse), after that frame's property writes.
    void setOnFinished(std::function<void()> cb) { onFinished_ = std::move(cb); }

    /// Fired for each event key crossed: (name, argsJson) — argsJson is ""
    /// when the key has no args, else the JSON payload stored at addClip.
    using EventCallback =
        std::function<void(const std::string& name, const std::string& argsJson)>;
    void setOnEvent(EventCallback cb) { onEvent_ = std::move(cb); }

    /// Deferred destruction (mirrors Tween): hidden from findClipPlayer
    /// immediately; SceneGraph erases it after the tick pass, so a player
    /// may destroy itself from its own event/finished callback.
    void markDestroyed() { destroyed_ = true; }
    bool destroyed() const { return destroyed_; }

    // --- Tick (SceneGraph::tickAnimations) ---

    void tick(float dtSec, SceneGraph& graph);

private:
    struct ActiveTrack {
        const AnimationClip::PropTrack* track = nullptr; // into the shared clip
        uint32_t nodeId = 0;
        uint32_t cursor = 0;         ///< cached key index (amortized O(1) walk)
        float startValue[4] = {};    ///< target's value captured at play()
        int fadeFromIndex = -1;      ///< matching track in fadeFrom_, -1 = none
        bool shadowed = false;       ///< (as fade source) matched by incoming
        float value[4] = {};         ///< sampled this tick
    };

    struct ActiveClip {
        std::shared_ptr<const AnimationClip> clip; // null = inactive
        std::string name;
        float time = 0.0f;
        float speed = 1.0f;
        int pingpongDir = 1;
        bool includeLeft = false;    ///< next event interval includes its left end
        bool finished = false;       ///< Loop::None reached its boundary
        std::vector<ActiveTrack> tracks;
    };

    /// Advance c.time by `delta` seconds (signed), honoring the loop mode.
    /// `fire` = invoke event callbacks (false for the crossfade source).
    /// Returns false when the walk must abort (an event callback restarted,
    /// stopped, sought, or destroyed this player).
    bool advanceClip(ActiveClip& c, float delta, bool fire);

    /// Fire event keys crossed by moving from `from` to `to` (dir = +1/-1;
    /// with dir < 0, from >= to and keys fire in descending time order).
    /// Consumes c.includeLeft. Same abort contract as advanceClip.
    bool fireEvents(ActiveClip& c, float from, float to, int dir);

    /// Sample every track of `c` at c.time into its value slot.
    void sampleTracks(ActiveClip& c);

    uint32_t id_;

    std::unordered_map<std::string,
                       std::shared_ptr<const AnimationClip>> clips_;

    ActiveClip current_;

    // Crossfade state: the previous clip keeps advancing (events muted)
    // while `w = fadeElapsed_/fadeTime_` ramps the incoming clip in.
    ActiveClip fadeFrom_;
    bool fading_ = false;
    float fadeTime_ = 0.0f;
    float fadeElapsed_ = 0.0f;

    bool paused_ = false;
    bool destroyed_ = false;
    uint64_t generation_ = 0;  // bumped by play/stop/seek; aborts event walks

    std::function<void()> onFinished_;
    EventCallback onEvent_;
};

} // namespace bro::scene
