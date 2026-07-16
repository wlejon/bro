#pragma once

#include <bromesh/animation/pose.h>
#include <bromesh/mesh_data.h>

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
/// Model: one BASE track (full-body clip) plus one optional masked LAYER
/// track on top (e.g. upper-body wave over a walk). play() on the base track
/// crossfades from whatever was playing over fadeTime seconds; the layer
/// slot replaces atomically. Not a state machine or blend tree — two slots.
///
/// Skeleton/clips are shared_ptr copies of the JS-side rigging objects, so
/// the player never dangles when the JS wrapper is GC'd. While the player is
/// inactive (never played, or stopped) it does not touch the node's palette —
/// manual setSkinningMatrices keeps working.
class AnimationPlayer {
public:
    explicit AnimationPlayer(SkinnedMeshNode& owner) : owner_(owner) {}

    // --- Setup ---

    void setSkeleton(std::shared_ptr<const bromesh::Skeleton> skel);
    const bromesh::Skeleton* skeleton() const { return skeleton_.get(); }

    /// Register a clip under `name`. Replaces an existing clip of the same
    /// name (a track currently playing the old clip keeps its reference).
    void addClip(const std::string& name,
                 std::shared_ptr<const bromesh::Animation> clip);
    bool hasClip(const std::string& name) const {
        return clips_.find(name) != clips_.end();
    }

    // --- Playback ---

    struct PlayOptions {
        bool  loop = true;
        float speed = 1.0f;
        float fadeTime = 0.0f;   // seconds; crossfade from the current pose
        float weight = 1.0f;     // blend weight vs what's underneath
        std::vector<uint8_t> mask; // non-empty → masked LAYER track (1 = bone
                                   // animated by this clip, 0 = untouched)
    };

    /// Start a clip. Empty mask → base track (crossfading over fadeTime from
    /// the current blended pose); non-empty mask → layer track (replaces any
    /// existing layer). Returns false if the clip or skeleton is missing.
    bool play(const std::string& name, const PlayOptions& opts);

    /// Fade the whole result (base + layer) to bind pose over fadeTime, then
    /// deactivate — after which manual setSkinningMatrices works again.
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

    float time() const { return base_.time; }
    /// Scrub the base track. Re-evaluates + re-stages the palette immediately
    /// (works while paused). Clamps/wraps like a tick would.
    void setTime(float t);

    float speed() const { return base_.speed; }
    void setSpeed(float s) { base_.speed = s; }

    /// Duration of the current base clip (0 when nothing is loaded).
    float duration() const { return base_.clip ? base_.clip->duration : 0.0f; }

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
    struct Track {
        std::shared_ptr<const bromesh::Animation> clip;
        std::string name;
        float time = 0.0f;
        float speed = 1.0f;
        float weight = 1.0f;
        bool  loop = true;
        bool  playing = false;   // false once a non-looping clip finishes
        bromesh::Pose pose;      // per-track scratch (evaluateInto, no alloc)

        void reset() { clip.reset(); name.clear(); time = 0; playing = false; }
        /// Advance time; returns true if a non-looping clip just finished.
        bool advance(float dt);
        void evaluate(const bromesh::Skeleton& skel) {
            bromesh::evaluateAnimationInto(skel, *clip, time, loop, pose);
        }
    };

    /// Evaluate all tracks at their current times, blend, stage the node's
    /// palette, and invalidate cached world matrices.
    void applyPose();
    void ensureWorldMatrices();

    SkinnedMeshNode& owner_;

    std::shared_ptr<const bromesh::Skeleton> skeleton_;
    std::unordered_map<std::string,
                       std::shared_ptr<const bromesh::Animation>> clips_;

    Track base_;
    Track fadeFrom_;             // crossfade source (previous base track)
    float fadeTime_ = 0.0f;
    float fadeElapsed_ = 0.0f;
    bool  fading_ = false;

    Track layer_;
    std::vector<uint8_t> layerMask_;
    bool  layerActive_ = false;

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
};

} // namespace bro::scene
