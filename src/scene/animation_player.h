#pragma once

#include <bromesh/animation/pose.h>
#include <bromesh/mesh_data.h>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace bro::scene {

class SkinnedMeshNode;

/// Skeletal animation player for a SkinnedMeshNode. Owns the whole per-frame
/// CPU rigging pipeline — evaluate clip(s) → blend → computeSkinningMatrices →
/// stage the node's palette — so a JS app that calls only play() gets animated
/// characters with zero per-frame JS. Ticked by SkinnedMeshNode::onTick from
/// SceneGraph::tickAnimations (windowed frame loop and headless virtual time
/// alike, so scripted tests are deterministic).
///
/// Model: one BASE track plus up to kMaxLayers ordered masked LAYER tracks
/// blended on top (e.g. upper-body wave over a walk). The base track plays
/// either a single clip or a registered blend space (1D/2D); play() on the
/// base crossfades from whatever was playing over fadeTime seconds. Layers
/// are addressed by slot via playLayer(); the legacy play(name, {mask})
/// form is slot 0. Blend spaces + layers cover locomotion + overlay needs;
/// the state machine (below) adds authored transitions on top.
///
/// Blend spaces: addBlendSpace1D/2D register a named set of clips at
/// parameter positions; play(name) makes the space the base track (a space
/// shadows a same-named clip). setBlendPos() moves the parameter instantly —
/// no internal smoothing; apps tween the parameter if they want easing.
/// Participating clips advance on one shared normalized phase (0..1 of each
/// clip's own duration), so gait cycles stay foot-aligned across the blend.
///
/// Skeleton/clips are shared_ptr copies of the JS-side rigging objects, so
/// the player never dangles when the JS wrapper is GC'd. While the player is
/// inactive (never played, or stopped) it does not touch the node's palette —
/// manual setSkinningMatrices keeps working.
///
/// State machine (the tier above): setStateMachine() registers named states
/// (each referencing a clip or blend space) plus authored transitions;
/// travel() follows the defined transition from the current state (wildcard
/// '*' fallback) using its fade — bro is code-first, so the APP decides when
/// to travel; there is no condition/expression language. travel() with no
/// defined transition warns and hard-switches (fade 0). Manual play()/stop()
/// SUSPENDS the machine (state becomes empty, definition retained); the next
/// travel() re-enters it. autoAdvance transitions fire when a non-looping
/// state's clip finishes; looping states never auto-advance.
///
/// Root motion (opt-in via setRootMotion): each tick, after the full blended
/// pose is produced and before skinning, the root bone's translation/yaw
/// delta is extracted (continuous across crossfades because the blended pose
/// is continuous; loop wraps are corrected with the clip's net-loop root
/// displacement so deltas telescope exactly), accumulated for
/// consumeRootMotion(), and removed from the pose (X/Z + yaw pinned to their
/// values at enable time; Y stays authored unless extractY) so the character
/// stays put while the app moves the node/physics body.
class AnimationPlayer {
public:
    /// Hard cap on simultaneously active layer slots. Bounded so the
    /// evaluate/blend path stays allocation-free and O(1) in state.
    static constexpr int kMaxLayers = 8;

    explicit AnimationPlayer(SkinnedMeshNode& owner) : owner_(owner) {}

    // --- Setup ---

    void setSkeleton(std::shared_ptr<const bromesh::Skeleton> skel);
    const bromesh::Skeleton* skeleton() const { return skeleton_.get(); }

    /// Register a clip under `name`. Replaces an existing clip of the same
    /// name (a track currently playing the old clip keeps its reference;
    /// blend spaces capture their clips at addBlendSpace time).
    void addClip(const std::string& name,
                 std::shared_ptr<const bromesh::Animation> clip);
    bool hasClip(const std::string& name) const {
        return clips_.find(name) != clips_.end();
    }

    /// One point of a blend space: a registered clip name at a parameter
    /// position. `timescale` compensates authored cadence differences: it
    /// scales this clip's contribution to the blended cycle duration
    /// (effective cycle = duration / timescale), not its sampling.
    struct BlendSpacePoint {
        std::string clip;
        float pos[2] = {0.0f, 0.0f};   // 1D uses pos[0]
        float timescale = 1.0f;
    };

    /// Register a 1D blend space (points sorted by position internally; the
    /// parameter clamps to [min,max]). Replaces a same-named space in place
    /// (a track playing it re-resolves weights next evaluate). Returns false
    /// if points is empty or any clip name is unregistered.
    bool addBlendSpace1D(const std::string& name,
                         std::vector<BlendSpacePoint> points);

    /// Register a 2D blend space. Evaluation blends the 3 nearest points by
    /// inverse-squared-distance weighting (normalized; a zero-distance point
    /// takes full weight, split evenly across coincident points) — simpler
    /// and more robust than Godot-style triangulation, at the cost of a
    /// weight discontinuity when the nearest-3 set changes; keep sample
    /// points sparse and well-spaced. Degenerate layouts (coincident, all
    /// collinear) are safe. Returns false as for addBlendSpace1D.
    bool addBlendSpace2D(const std::string& name,
                         std::vector<BlendSpacePoint> points);

    bool hasBlendSpace(const std::string& name) const {
        return spaces_.find(name) != spaces_.end();
    }

    /// Move a blend space's parameter (y ignored for 1D; x clamped to the
    /// 1D range). Takes effect immediately — re-poses this frame, even while
    /// paused — with no internal smoothing. Returns false for an unknown
    /// space.
    bool setBlendPos(const std::string& name, float x, float y = 0.0f);

    // --- Playback ---

    struct PlayOptions {
        bool  loop = true;        // ignored by blend spaces (always looping)
        float speed = 1.0f;
        float fadeTime = 0.0f;   // base: crossfade seconds; layer: weight fade-in
        float weight = 1.0f;     // blend weight vs what's underneath
        std::vector<uint8_t> mask; // per-bone 1 = animated by this track.
                                   // play(): non-empty → legacy layer slot 0.
                                   // playLayer(): empty = whole body.
    };

    /// Start a clip or blend space on the base track, crossfading over
    /// fadeTime from the current blended pose (blend spaces shadow
    /// same-named clips). A non-empty opts.mask routes to playLayer(0) for
    /// back-compat with the old single-layer API. Returns false if the
    /// name or skeleton is missing.
    bool play(const std::string& name, const PlayOptions& opts);

    /// Start a clip on layer `slot` (0..kMaxLayers-1), replacing that slot
    /// atomically. opts.mask selects the affected bones (empty = all);
    /// opts.fadeTime fades the layer's weight in from 0. Layers blend over
    /// the base in ascending slot order. Blend spaces are base-track only.
    /// Returns false for a bad slot or unknown clip.
    bool playLayer(int slot, const std::string& name, const PlayOptions& opts);

    /// Fade layer `slot` out over fadeTime seconds (0 = immediately) and
    /// free the slot.
    void stopLayer(int slot, float fadeTime = 0.0f);

    /// Set a layer's blend weight (takes effect immediately; multiplied by
    /// any in-progress fade). Returns false for a bad/empty slot.
    bool setLayerWeight(int slot, float weight);

    // --- State machine (code-first travel(); see class comment) ---

    struct StateDef {
        std::string name;
        std::string source;      // registered clip or blend-space name
        float speed = 1.0f;
        bool  loop = true;       // clips only; blend spaces always loop
    };
    struct TransitionDef {
        std::string from;        // state name, or "*" wildcard
        std::string to;
        float fade = 0.0f;       // crossfade seconds (default: hard switch)
        bool  autoAdvance = false; // fire when `from`'s non-looping clip ends
        bool  syncPhase = false;   // carry normalized phase across the switch
                                   // (only when both states are cycles)
    };
    struct StateMachineDef {
        std::vector<StateDef> states;
        std::vector<TransitionDef> transitions;
        std::string initial;     // empty = first state
    };

    /// Install a state machine and enter its initial state immediately
    /// (fade 0). Replaces any previous machine. Returns false (with *err
    /// set) on validation failure: empty/duplicate state names, a source
    /// that is neither a registered clip nor blend space, a transition
    /// endpoint naming no state, or an unknown initial state.
    bool setStateMachine(StateMachineDef def, std::string* err = nullptr);
    bool hasStateMachine() const { return !machineStates_.empty(); }

    /// Current state name; empty while suspended (manual play()/stop() took
    /// over) or when no machine is installed.
    const std::string& currentState() const;

    /// Transition to `stateName` via the defined transition from the current
    /// state (exact match first, then a "*" wildcard). No defined transition:
    /// warns and switches directly with fade 0. travel() to the current state
    /// is a no-op. Returns false only for an unknown state / no machine.
    bool travel(const std::string& stateName);

    /// Fired after every machine transition (travel or autoAdvance) with
    /// (fromState, toState); fromState is empty when re-entering from the
    /// suspended state. Not fired for the initial state on setStateMachine.
    using StateChangedCallback =
        std::function<void(const std::string& from, const std::string& to)>;
    void setOnStateChanged(StateChangedCallback cb) {
        onStateChanged_ = std::move(cb);
    }

    // --- Root motion (see class comment) ---

    struct RootMotionOptions {
        bool enabled = false;
        int bone = -1;           // explicit bone index; -1 = boneName / auto
        std::string boneName;    // explicit bone name; empty = auto-detect
                                 // (first parentless bone, else bone 0)
        bool extractY = false;   // also extract+remove Y (default: Y stays
                                 // authored so jumps/crouches render)
    };
    /// Enable/disable extraction. Requires a skeleton when enabling; returns
    /// false for a missing skeleton or an unknown/out-of-range bone. Resets
    /// the accumulator and the delta baseline (the tick after enabling never
    /// spikes).
    bool setRootMotion(const RootMotionOptions& opts);
    bool rootMotionEnabled() const { return rmEnabled_; }
    int  rootMotionBone() const { return rmBone_; }

    /// Accumulated root displacement in MODEL space (the clip's authoring
    /// space, before Skeleton::rootTransform and the node's own TRS) since
    /// the last call; yaw is radians about +Y. Resets to zero on read.
    struct RootMotionDelta {
        float translation[3] = {0.0f, 0.0f, 0.0f};
        float yaw = 0.0f;
    };
    RootMotionDelta consumeRootMotion();

    /// Fade the whole result (base + layers) to bind pose over fadeTime,
    /// then deactivate — after which manual setSkinningMatrices works again.
    /// fadeTime 0 = immediate bind pose + deactivate.
    void stop(float fadeTime = 0.0f);

    void pause()  { paused_ = true; }
    void resume() { if (active_) paused_ = false; }
    bool isPaused() const { return paused_; }

    /// True while a base clip is advancing (active, not paused, and — for a
    /// non-looping clip — not yet finished-and-holding).
    bool isPlaying() const { return active_ && !paused_ && base_.playing; }

    /// True while the player drives the palette (even paused or holding the
    /// last frame of a finished one-shot).
    bool isActive() const { return active_; }

    const std::string& currentClip() const { return base_.name; }

    /// Base clock in seconds. For a blend space this is phase × the current
    /// blended cycle duration.
    float time() const { return base_.time; }
    /// Scrub the base track. Re-evaluates + re-stages the palette immediately
    /// (works while paused). Clamps/wraps like a tick would; for a blend
    /// space, scrubs the shared phase (t / blended cycle duration).
    void setTime(float t);

    float speed() const { return base_.speed; }
    void setSpeed(float s) { base_.speed = s; }

    /// Duration of the current base clip; for a blend space, the current
    /// weighted cycle duration (0 when nothing is loaded).
    float duration() const;

    // --- Introspection (cheap; for tests, HUDs, and part-3 tooling) ---

    struct BlendState {
        struct ClipWeight { std::string name; float weight; };
        /// The base track's current composition — during a crossfade both
        /// the outgoing and incoming sources appear, weights summing to 1.
        std::vector<ClipWeight> clips;
        /// Blend space: the shared normalized phase (0..1). Single clip:
        /// time / duration.
        float phase = 0.0f;
        bool  hasPos = false;      // base track is a blend space
        bool  is2D = false;
        float pos[2] = {0.0f, 0.0f};
        struct LayerState { int slot; std::string name; float weight; float phase; };
        std::vector<LayerState> layers;   // active layers, ascending slot
        /// Current state-machine state (empty: suspended or no machine).
        std::string state;
    };
    BlendState blendState() const;

    /// Fired once when a non-looping track reaches its end (base or layer),
    /// with the clip name. Invoked from the tick (main/JS thread).
    using FinishedCallback = std::function<void(const std::string&)>;
    void setOnFinished(FinishedCallback cb) { onFinished_ = std::move(cb); }

    // --- Tick (SkinnedMeshNode::onTick) ---

    void tick(float dtSec);

    // --- Verification / attachment seam ---

    /// Current posed bone matrix in MODEL space (the skinned mesh's local
    /// space, before the node's own TRS): world(bone) as computed by
    /// bromesh::computeWorldMatrices over the current blended pose. Before
    /// the first tick this is the bind pose. 16 floats, column-major.
    /// Returns false for an out-of-range bone or missing skeleton.
    bool boneWorldMatrix(int boneIndex, float out[16]);
    bool boneWorldMatrix(const std::string& boneName, float out[16]);

private:
    struct BlendSpaceEntry {
        std::shared_ptr<const bromesh::Animation> clip;
        std::string name;
        float px = 0.0f, py = 0.0f;
        float timescale = 1.0f;
        bromesh::Pose scratch;   // per-entry eval scratch (no per-frame alloc)
        // Root motion: this clip's net root displacement over one loop
        // (rootAt(duration) - rootAt(0)), for wrap correction. Computed
        // lazily while root motion is enabled.
        float netT[3] = {0.0f, 0.0f, 0.0f};
        float netYaw = 0.0f;
        bool  netValid = false;
    };
    struct BlendSpace {
        bool is2D = false;
        std::vector<BlendSpaceEntry> entries;  // 1D: sorted by px
        float posX = 0.0f, posY = 0.0f;        // shared parameter
        float minX = 0.0f, maxX = 0.0f;        // 1D clamp range
    };

    struct Track {
        std::shared_ptr<const bromesh::Animation> clip;  // single-clip mode
        BlendSpace* space = nullptr;                     // blend-space mode
        std::string name;
        float time = 0.0f;
        float phase = 0.0f;          // blend space: shared normalized phase
        float speed = 1.0f;
        float weight = 1.0f;
        bool  loop = true;
        bool  playing = false;   // false once a non-looping clip finishes
        bromesh::Pose pose;      // per-track scratch (evaluateInto, no alloc)

        // Blend-space working set, refreshed by updateBlendWeights(): the
        // (≤3) participating entries, their normalized weights, and the
        // weighted cycle duration.
        int   activeIdx[3] = {-1, -1, -1};
        float activeW[3] = {0.0f, 0.0f, 0.0f};
        int   activeCount = 0;
        float cachedCycleDur = 0.0f;

        // Signed loop wraps crossed by the last advance() (clip loop or the
        // space's shared phase). Consumed (zeroed) by root-motion extraction.
        int   wrapCount = 0;
        // Clip mode: net root displacement over one loop (see
        // BlendSpaceEntry::netT); space mode uses the entries' nets.
        float rootNetT[3] = {0.0f, 0.0f, 0.0f};
        float rootNetYaw = 0.0f;
        bool  rootNetValid = false;

        bool valid() const { return clip || space; }
        void reset() {
            clip.reset(); space = nullptr; name.clear();
            time = 0; phase = 0; playing = false;
            activeCount = 0; cachedCycleDur = 0;
            wrapCount = 0; rootNetValid = false;
        }
        /// Blend-space mode: recompute activeIdx/W + cachedCycleDur from the
        /// space's current parameter. No-op in clip mode.
        void updateBlendWeights();
        /// Advance time; returns true if a non-looping clip just finished
        /// (blend spaces always loop).
        bool advance(float dt);
        void evaluate(const bromesh::Skeleton& skel);
    };

    struct Layer {
        Track track;
        std::vector<uint8_t> mask;   // empty = all bones
        bool  active = false;
        float fadeInTime = 0.0f, fadeInElapsed = 0.0f;
        bool  fadingOut = false;
        float fadeOutTime = 0.0f, fadeOutElapsed = 0.0f;
        float fadeOutStartW = 0.0f;  // effective weight when stopLayer() hit

        float effectiveWeight() const;
        void reset() {
            track.reset(); mask.clear(); active = false;
            fadeInTime = fadeInElapsed = 0.0f;
            fadingOut = false; fadeOutTime = fadeOutElapsed = 0.0f;
            fadeOutStartW = 0.0f;
        }
    };

    bool addBlendSpace(const std::string& name,
                       std::vector<BlendSpacePoint> points, bool is2D);
    /// The core of play(): starts a clip/space on the base track WITHOUT
    /// touching the machine (play() suspends it; enterState() keeps it).
    bool startBase(const std::string& name, const PlayOptions& opts);
    BlendSpace* findSpace(const std::string& name);
    /// Size a blend-space track's scratch poses to the skeleton (play time).
    void presizeSpaceScratch(BlendSpace& sp);
    void appendTrackWeights(const Track& t, float scale,
                            std::vector<BlendState::ClipWeight>& out) const;

    /// Evaluate all tracks at their current times, blend, stage the node's
    /// palette, and invalidate cached world matrices.
    void applyPose();
    void ensureWorldMatrices();

    SkinnedMeshNode& owner_;

    std::shared_ptr<const bromesh::Skeleton> skeleton_;
    std::unordered_map<std::string,
                       std::shared_ptr<const bromesh::Animation>> clips_;
    std::unordered_map<std::string, std::unique_ptr<BlendSpace>> spaces_;

    Track base_;
    Track fadeFrom_;             // crossfade source (previous base track)
    float fadeTime_ = 0.0f;
    float fadeElapsed_ = 0.0f;
    bool  fading_ = false;

    std::array<Layer, kMaxLayers> layers_;
    bool  anyLayerActive_ = false;

    bool  stopping_ = false;     // fading everything to bind pose
    float stopFadeTime_ = 0.0f;
    float stopElapsed_ = 0.0f;

    bool active_ = false;
    bool paused_ = false;

    bromesh::Pose bindPose_;
    bromesh::Pose result_;       // final blended pose (source for palette + queries)
    std::vector<float> palette_;
    std::vector<float> worldMats_;
    bool worldMatsDirty_ = true;

    FinishedCallback onFinished_;

    // --- State machine (per-frame state is just machineCurrent_) ---
    struct MachineState {
        std::string name;
        std::string source;
        float speed = 1.0f;
        bool  loop = true;
    };
    struct MachineTransition {
        int   from = -1;         // index into machineStates_; -1 = wildcard
        int   to = 0;
        float fade = 0.0f;
        bool  autoAdvance = false;
        bool  syncPhase = false;
    };
    std::vector<MachineState> machineStates_;
    std::vector<MachineTransition> machineTransitions_;
    int machineCurrent_ = -1;    // -1 = suspended / no machine
    StateChangedCallback onStateChanged_;

    int  findState(const std::string& name) const;
    /// Defined transition from `from` to `to`: exact match first, then a
    /// wildcard (from == -1). Returns index into machineTransitions_ or -1.
    int  findTransition(int from, int to) const;
    /// First autoAdvance transition out of `from` (exact, then wildcard).
    int  findAutoTransition(int from) const;
    /// Switch the base track to state `idx` (crossfade over `fade`), with
    /// optional phase carry-over, then fire onStateChanged_.
    void enterState(int idx, float fade, bool syncPhase, bool fireCallback);

    // --- Root motion ---
    bool  rmEnabled_ = false;
    int   rmBone_ = -1;
    bool  rmExtractY_ = false;
    bool  rmInTick_ = false;     // accumulate deltas only from tick()'s pose
    bool  rmHavePrev_ = false;   // baseline sample valid (rebased on any
                                 // non-tick re-pose: play/seek/setBlendPos)
    bool  rmHaveRef_ = false;    // pin target captured (at enable time)
    float rmPrevT_[3] = {0.0f, 0.0f, 0.0f};
    float rmPrevYaw_ = 0.0f;
    float rmRefT_[3] = {0.0f, 0.0f, 0.0f};
    float rmRefYaw_ = 0.0f;
    float rmAccumT_[3] = {0.0f, 0.0f, 0.0f};
    float rmAccumYaw_ = 0.0f;
    bromesh::Pose rmScratch_;    // net-loop eval scratch (setup-time only)

    static float yawOfQuat(const float q[4]);   // xyzw, yaw about +Y
    static float wrapPi(float a);               // wrap to (-pi, pi]
    /// rootAt(duration) - rootAt(0) of `clip`'s root bone (rmBone_).
    void computeClipRootNet(const bromesh::Animation& clip,
                            float outT[3], float* outYaw);
    /// Fill the track's (or its space entries') net-loop displacement.
    void computeTrackRootNet(Track& t);
    /// Add `w` × wrapCount × net-loop displacement of `t` to the deltas.
    void addWrapCorrection(const Track& t, float w,
                           float* dx, float* dy, float* dz, float* dyaw) const;
    /// applyPose() tail: accumulate the root delta (tick only) and pin the
    /// root in result_ so the character stays at its enable-time spot.
    void extractRootMotion();
};

} // namespace bro::scene
