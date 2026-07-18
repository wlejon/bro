#include "scene/animation_player.h"
#include "scene/skinned_mesh_node.h"
#include "util/log.h"

#include <algorithm>
#include <cmath>

namespace bro::scene {

// ---------------------------------------------------------------------------
// Track
// ---------------------------------------------------------------------------

void AnimationPlayer::Track::updateBlendWeights() {
    if (!space) return;
    const auto& entries = space->entries;
    int n = (int)entries.size();
    activeCount = 0;
    cachedCycleDur = 0.0f;
    if (n == 0) return;

    if (n == 1) {
        activeIdx[0] = 0; activeW[0] = 1.0f; activeCount = 1;
    } else if (!space->is2D) {
        // 1D: the two neighbors of the (already clamped) parameter.
        float x = std::min(std::max(space->posX, space->minX), space->maxX);
        int i = 0;
        while (i + 2 < n && entries[i + 1].px <= x) ++i;
        float x0 = entries[i].px, x1 = entries[i + 1].px;
        float t = (x1 > x0) ? (x - x0) / (x1 - x0) : 0.0f;
        t = std::min(std::max(t, 0.0f), 1.0f);
        activeIdx[0] = i;     activeW[0] = 1.0f - t;
        activeIdx[1] = i + 1; activeW[1] = t;
        activeCount = 2;
    } else {
        // 2D: 3 nearest points, inverse-squared-distance weights.
        int   best[3] = {-1, -1, -1};
        float bd[3] = {1e30f, 1e30f, 1e30f};
        for (int k = 0; k < n; ++k) {
            float dx = entries[k].px - space->posX;
            float dy = entries[k].py - space->posY;
            float d2 = dx * dx + dy * dy;
            if (d2 < bd[0]) {
                bd[2] = bd[1]; best[2] = best[1];
                bd[1] = bd[0]; best[1] = best[0];
                bd[0] = d2;    best[0] = k;
            } else if (d2 < bd[1]) {
                bd[2] = bd[1]; best[2] = best[1];
                bd[1] = d2;    best[1] = k;
            } else if (d2 < bd[2]) {
                bd[2] = d2;    best[2] = k;
            }
        }
        int m = std::min(n, 3);
        constexpr float kZero = 1e-12f;
        int zeros = 0;
        for (int k = 0; k < m; ++k)
            if (bd[k] <= kZero) ++zeros;
        float sum = 0.0f;
        for (int k = 0; k < m; ++k) {
            activeIdx[k] = best[k];
            if (zeros > 0) {
                // On (or numerically at) a sample point: that point wins;
                // coincident points split evenly. Never divides by zero.
                activeW[k] = (bd[k] <= kZero) ? 1.0f / (float)zeros : 0.0f;
            } else {
                activeW[k] = 1.0f / bd[k];
                sum += activeW[k];
            }
        }
        if (zeros == 0)
            for (int k = 0; k < m; ++k) activeW[k] /= sum;
        activeCount = m;
    }

    for (int k = 0; k < activeCount; ++k) {
        const auto& e = entries[activeIdx[k]];
        float dur = e.clip ? e.clip->duration : 0.0f;
        float ts = (e.timescale > 0.0f) ? e.timescale : 1.0f;
        cachedCycleDur += activeW[k] * (dur / ts);
    }
}

bool AnimationPlayer::Track::advance(float dt) {
    wrapCount = 0;   // re-armed every advance; consumed by root motion
    if (space) {
        // Blend spaces always loop: one shared normalized phase advances at
        // the rate of the current weighted cycle duration, so participating
        // gait cycles stay foot-aligned across the blend.
        updateBlendWeights();
        if (!playing) return false;
        if (cachedCycleDur > 1e-6f) {
            phase += dt * speed / cachedCycleDur;
            float f = std::floor(phase);
            wrapCount = (int)f;
            phase -= f;
        }
        time = phase * cachedCycleDur;
        return false;
    }
    if (!clip || !playing) return false;
    time += dt * speed;
    float dur = clip->duration;
    if (dur <= 0.0f) { time = 0.0f; return false; }
    if (loop) {
        float f = std::floor(time / dur);
        wrapCount = (int)f;
        time -= f * dur;
        // Float guard: keep time in [0, dur) and the wrap count consistent.
        if (time >= dur) { time -= dur; ++wrapCount; }
        if (time < 0.0f) { time += dur; --wrapCount; }
        return false;
    }
    // Non-looping: clamp and finish once past either end (negative speed
    // finishes at 0).
    if (time >= dur) { time = dur; playing = false; return true; }
    if (time <= 0.0f && speed < 0.0f) { time = 0.0f; playing = false; return true; }
    return false;
}

void AnimationPlayer::Track::evaluate(const bromesh::Skeleton& skel) {
    if (space) {
        // Recompute here too: setBlendPos() re-poses without an advance.
        updateBlendWeights();
        if (activeCount == 0) return;
        const bromesh::Pose* poses[3];
        for (int k = 0; k < activeCount; ++k) {
            auto& e = space->entries[activeIdx[k]];
            bromesh::evaluateAnimationInto(skel, *e.clip,
                                           phase * e.clip->duration,
                                           /*loop=*/true, e.scratch);
            poses[k] = &e.scratch;
        }
        if (activeCount == 1) {
            pose.data = poses[0]->data;   // exact copy, no blend
        } else {
            bromesh::blendPosesN(poses, activeW, (size_t)activeCount, pose);
        }
        return;
    }
    bromesh::evaluateAnimationInto(skel, *clip, time, loop, pose);
}

float AnimationPlayer::Layer::effectiveWeight() const {
    if (fadingOut) {
        float t = fadeOutTime > 0.0f
            ? std::min(fadeOutElapsed / fadeOutTime, 1.0f) : 1.0f;
        return fadeOutStartW * (1.0f - t);
    }
    float w = track.weight;
    if (fadeInTime > 0.0f)
        w *= std::min(fadeInElapsed / fadeInTime, 1.0f);
    return w;
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

void AnimationPlayer::setSkeleton(std::shared_ptr<const bromesh::Skeleton> skel) {
    skeleton_ = std::move(skel);
    if (skeleton_) {
        bindPose_ = bromesh::bindPose(*skeleton_);
        result_ = bindPose_;
    } else {
        bindPose_ = {};
        result_ = {};
    }
    worldMatsDirty_ = true;
}

void AnimationPlayer::addClip(const std::string& name,
                              std::shared_ptr<const bromesh::Animation> clip) {
    if (!clip) return;
    clips_[name] = std::move(clip);
}

bool AnimationPlayer::addBlendSpace(const std::string& name,
                                    std::vector<BlendSpacePoint> points,
                                    bool is2D) {
    if (points.empty()) return false;
    for (const auto& p : points)
        if (clips_.find(p.clip) == clips_.end()) return false;

    // Replace in place so a track currently playing this space stays valid
    // (weights re-resolve on the next evaluate).
    auto& slot = spaces_[name];
    if (!slot) slot = std::make_unique<BlendSpace>();
    BlendSpace& sp = *slot;
    sp.is2D = is2D;
    sp.entries.clear();
    sp.entries.reserve(points.size());
    for (auto& p : points) {
        BlendSpaceEntry e;
        e.clip = clips_[p.clip];
        e.name = p.clip;
        e.px = p.pos[0];
        e.py = is2D ? p.pos[1] : 0.0f;
        e.timescale = (p.timescale > 0.0f) ? p.timescale : 1.0f;
        sp.entries.push_back(std::move(e));
    }
    if (!is2D) {
        std::stable_sort(sp.entries.begin(), sp.entries.end(),
                         [](const BlendSpaceEntry& a, const BlendSpaceEntry& b) {
                             return a.px < b.px;
                         });
        sp.minX = sp.entries.front().px;
        sp.maxX = sp.entries.back().px;
        sp.posX = sp.minX;
        sp.posY = 0.0f;
    } else {
        sp.posX = sp.entries.front().px;
        sp.posY = sp.entries.front().py;
    }
    if (skeleton_) presizeSpaceScratch(sp);
    return true;
}

bool AnimationPlayer::addBlendSpace1D(const std::string& name,
                                      std::vector<BlendSpacePoint> points) {
    return addBlendSpace(name, std::move(points), /*is2D=*/false);
}

bool AnimationPlayer::addBlendSpace2D(const std::string& name,
                                      std::vector<BlendSpacePoint> points) {
    return addBlendSpace(name, std::move(points), /*is2D=*/true);
}

AnimationPlayer::BlendSpace* AnimationPlayer::findSpace(const std::string& name) {
    auto it = spaces_.find(name);
    return it != spaces_.end() ? it->second.get() : nullptr;
}

void AnimationPlayer::presizeSpaceScratch(BlendSpace& sp) {
    for (auto& e : sp.entries)
        if (e.scratch.boneCount() != bindPose_.boneCount())
            e.scratch = bindPose_;
}

bool AnimationPlayer::setBlendPos(const std::string& name, float x, float y) {
    BlendSpace* sp = findSpace(name);
    if (!sp) return false;
    if (!sp->is2D) {
        sp->posX = std::min(std::max(x, sp->minX), sp->maxX);
        sp->posY = 0.0f;
    } else {
        sp->posX = x;
        sp->posY = y;
    }
    // Instant re-pose (works while paused) if a live track reads this space.
    if (active_ && (base_.space == sp || (fading_ && fadeFrom_.space == sp)))
        applyPose();
    return true;
}

// ---------------------------------------------------------------------------
// Playback
// ---------------------------------------------------------------------------

bool AnimationPlayer::play(const std::string& name, const PlayOptions& opts) {
    if (!skeleton_) return false;

    if (!opts.mask.empty())
        return playLayer(0, name, opts);   // legacy single-layer form

    bool ok = startBase(name, opts);
    // Manual base play takes over from the state machine: suspend it (the
    // definition stays; travel() re-enters). Layers coexist and never
    // suspend.
    if (ok) machineCurrent_ = -1;
    return ok;
}

bool AnimationPlayer::startBase(const std::string& name,
                                const PlayOptions& opts) {
    BlendSpace* sp = findSpace(name);      // spaces shadow same-named clips
    std::shared_ptr<const bromesh::Animation> clip;
    if (!sp) {
        auto it = clips_.find(name);
        if (it == clips_.end()) return false;
        clip = it->second;
    }

    if (active_ && base_.valid() && opts.fadeTime > 0.0f) {
        fadeFrom_ = std::move(base_);
        fading_ = true;
        fadeTime_ = opts.fadeTime;
        fadeElapsed_ = 0.0f;
    } else {
        fading_ = false;
        fadeFrom_.reset();
    }
    base_.reset();
    base_.name = name;
    base_.speed = opts.speed;
    base_.weight = opts.weight;
    if (sp) {
        base_.space = sp;
        base_.loop = true;                 // blend spaces always loop
        presizeSpaceScratch(*sp);
        if (base_.pose.boneCount() != bindPose_.boneCount())
            base_.pose = bindPose_;        // pre-size the blend target
    } else {
        base_.clip = std::move(clip);
        base_.loop = opts.loop;
    }
    base_.playing = true;

    if (rmEnabled_) computeTrackRootNet(base_);

    stopping_ = false;
    active_ = true;
    paused_ = false;
    applyPose();
    return true;
}

bool AnimationPlayer::playLayer(int slot, const std::string& name,
                                const PlayOptions& opts) {
    if (!skeleton_) return false;
    if (slot < 0 || slot >= kMaxLayers) return false;
    auto it = clips_.find(name);
    if (it == clips_.end()) return false;  // layers play clips, not spaces

    Layer& L = layers_[slot];
    L.reset();
    L.track.clip = it->second;
    L.track.name = name;
    L.track.speed = opts.speed;
    L.track.weight = opts.weight;
    L.track.loop = opts.loop;
    L.track.playing = true;
    L.mask = opts.mask;
    if (!L.mask.empty()) L.mask.resize(skeleton_->bones.size(), 0);
    L.fadeInTime = opts.fadeTime;
    L.active = true;
    anyLayerActive_ = true;

    stopping_ = false;
    active_ = true;
    paused_ = false;
    applyPose();
    return true;
}

void AnimationPlayer::stopLayer(int slot, float fadeTime) {
    if (slot < 0 || slot >= kMaxLayers) return;
    Layer& L = layers_[slot];
    if (!L.active) return;
    if (fadeTime > 0.0f) {
        L.fadeOutStartW = L.effectiveWeight();
        L.fadingOut = true;
        L.fadeOutTime = fadeTime;
        L.fadeOutElapsed = 0.0f;
        return;
    }
    L.reset();
    anyLayerActive_ = false;
    for (const auto& other : layers_)
        if (other.active) { anyLayerActive_ = true; break; }
    if (active_) applyPose();
}

bool AnimationPlayer::setLayerWeight(int slot, float weight) {
    if (slot < 0 || slot >= kMaxLayers) return false;
    Layer& L = layers_[slot];
    if (!L.active) return false;
    L.track.weight = weight;
    if (active_) applyPose();
    return true;
}

void AnimationPlayer::stop(float fadeTime) {
    if (!active_) return;
    machineCurrent_ = -1;   // stop() suspends the machine like manual play()
    if (fadeTime > 0.0f) {
        stopping_ = true;
        stopFadeTime_ = fadeTime;
        stopElapsed_ = 0.0f;
        paused_ = false;
        return;
    }
    // Immediate: back to bind pose, then hands-off (manual palette wins).
    base_.reset();
    fadeFrom_.reset();
    for (auto& L : layers_) L.reset();
    fading_ = false;
    anyLayerActive_ = false;
    stopping_ = false;
    paused_ = false;
    if (skeleton_) {
        result_ = bindPose_;
        bromesh::computeSkinningMatrices(*skeleton_, result_, palette_);
        owner_.setSkinningMatrices(palette_.data(), palette_.size() / 16);
        worldMatsDirty_ = true;
    }
    active_ = false;
}

float AnimationPlayer::duration() const {
    if (base_.space) return base_.cachedCycleDur;
    return base_.clip ? base_.clip->duration : 0.0f;
}

void AnimationPlayer::setTime(float t) {
    if (base_.space) {
        base_.updateBlendWeights();
        float dur = base_.cachedCycleDur;
        if (dur > 1e-6f) {
            base_.phase = t / dur;
            base_.phase -= std::floor(base_.phase);
            base_.time = base_.phase * dur;
        }
        if (active_) applyPose();
        return;
    }
    if (!base_.clip) return;
    float dur = base_.clip->duration;
    if (dur > 0.0f) {
        if (base_.loop) {
            t = std::fmod(t, dur);
            if (t < 0.0f) t += dur;
        } else {
            t = std::max(0.0f, std::min(t, dur));
            // Scrubbing back into range re-arms a finished one-shot.
            if (t < dur) base_.playing = true;
        }
    }
    base_.time = t;
    if (active_) applyPose();
}

// ---------------------------------------------------------------------------
// Tick
// ---------------------------------------------------------------------------

void AnimationPlayer::tick(float dtSec) {
    if (!active_ || paused_ || !skeleton_) return;

    // Advance clocks, collecting one-shot completions to fire after the
    // palette is staged (a callback may immediately play() something else).
    std::vector<std::string> finished;
    bool baseFinished = base_.advance(dtSec);
    if (baseFinished) finished.push_back(base_.name);
    if (fading_) {
        fadeFrom_.advance(dtSec); // fade source finishing is not an event
        fadeElapsed_ += dtSec;
        if (fadeElapsed_ >= fadeTime_) {
            fading_ = false;
            fadeFrom_.reset();
        }
    }
    if (anyLayerActive_) {
        bool any = false;
        for (auto& L : layers_) {
            if (!L.active) continue;
            if (L.track.advance(dtSec)) {
                // One-shot layer expires; looping layers persist.
                finished.push_back(L.track.name);
                L.reset();
                continue;
            }
            if (L.fadeInTime > 0.0f && L.fadeInElapsed < L.fadeInTime)
                L.fadeInElapsed += dtSec;
            if (L.fadingOut) {
                L.fadeOutElapsed += dtSec;
                if (L.fadeOutElapsed >= L.fadeOutTime) { L.reset(); continue; }
            }
            any = true;
        }
        anyLayerActive_ = any;
    }
    if (stopping_) stopElapsed_ += dtSec;

    rmInTick_ = true;    // root deltas accumulate only from the tick's pose
    applyPose();
    rmInTick_ = false;

    if (stopping_ && stopElapsed_ >= stopFadeTime_) {
        stop(0.0f);  // lands exactly on bind pose and deactivates
    }

    // autoAdvance: a machine state whose non-looping clip just ended follows
    // its authored transition (before onFinished, so the callback observes
    // the post-transition state).
    if (baseFinished && active_ && machineCurrent_ >= 0) {
        int tr = findAutoTransition(machineCurrent_);
        if (tr >= 0) {
            const MachineTransition& t = machineTransitions_[tr];
            enterState(t.to, t.fade, t.syncPhase, /*fireCallback=*/true);
        }
    }

    if (onFinished_) {
        for (const auto& n : finished) onFinished_(n);
    }
}

void AnimationPlayer::applyPose() {
    if (!skeleton_ || !active_) return;

    // Base (with optional crossfade from the previous base track).
    if (base_.valid()) {
        base_.evaluate(*skeleton_);
        if (fading_ && fadeFrom_.valid()) {
            fadeFrom_.evaluate(*skeleton_);
            result_ = fadeFrom_.pose;
            float alpha = fadeTime_ > 0.0f
                ? std::min(fadeElapsed_ / fadeTime_, 1.0f) : 1.0f;
            bromesh::blendPoses(result_, base_.pose, alpha);
        } else if (base_.weight < 1.0f) {
            result_ = bindPose_;
            bromesh::blendPoses(result_, base_.pose, base_.weight);
        } else {
            result_ = base_.pose;
        }
    } else {
        result_ = bindPose_;
    }

    // Masked layers on top, ascending slot order.
    if (anyLayerActive_) {
        for (auto& L : layers_) {
            if (!L.active || !L.track.clip) continue;
            L.track.evaluate(*skeleton_);
            float w = L.effectiveWeight();
            if (w <= 0.0f) continue;
            bromesh::blendPoses(result_, L.track.pose, w,
                                L.mask.empty() ? nullptr : L.mask.data());
        }
    }

    // Stop-fade: pull the whole result toward bind pose.
    if (stopping_ && stopFadeTime_ > 0.0f) {
        float alpha = std::min(stopElapsed_ / stopFadeTime_, 1.0f);
        bromesh::blendPoses(result_, bindPose_, alpha);
    }

    // Root motion: extract from the final blended pose (continuous through
    // crossfades) and pin the root before skinning.
    if (rmEnabled_) extractRootMotion();

    bromesh::computeSkinningMatrices(*skeleton_, result_, palette_);
    owner_.setSkinningMatrices(palette_.data(), palette_.size() / 16);
    worldMatsDirty_ = true;
}

// ---------------------------------------------------------------------------
// Introspection
// ---------------------------------------------------------------------------

void AnimationPlayer::appendTrackWeights(
        const Track& t, float scale,
        std::vector<BlendState::ClipWeight>& out) const {
    if (scale <= 0.0f || !t.valid()) return;
    if (t.space) {
        for (int k = 0; k < t.activeCount; ++k) {
            if (t.activeW[k] <= 0.0f) continue;
            out.push_back({t.space->entries[t.activeIdx[k]].name,
                           t.activeW[k] * scale});
        }
    } else {
        out.push_back({t.name, scale});
    }
}

AnimationPlayer::BlendState AnimationPlayer::blendState() const {
    BlendState s;
    s.state = currentState();
    if (!active_) return s;

    float alpha = 1.0f;
    if (fading_ && fadeFrom_.valid()) {
        alpha = fadeTime_ > 0.0f
            ? std::min(fadeElapsed_ / fadeTime_, 1.0f) : 1.0f;
        appendTrackWeights(fadeFrom_, 1.0f - alpha, s.clips);
    }
    appendTrackWeights(base_, alpha, s.clips);

    if (base_.space) {
        s.phase = base_.phase;
        s.hasPos = true;
        s.is2D = base_.space->is2D;
        s.pos[0] = base_.space->posX;
        s.pos[1] = base_.space->posY;
    } else if (base_.clip && base_.clip->duration > 0.0f) {
        s.phase = base_.time / base_.clip->duration;
    }

    for (int i = 0; i < kMaxLayers; ++i) {
        const Layer& L = layers_[i];
        if (!L.active || !L.track.clip) continue;
        float lphase = L.track.clip->duration > 0.0f
            ? L.track.time / L.track.clip->duration : 0.0f;
        s.layers.push_back({i, L.track.name, L.effectiveWeight(), lphase});
    }
    return s;
}

// ---------------------------------------------------------------------------
// State machine
// ---------------------------------------------------------------------------

bool AnimationPlayer::setStateMachine(StateMachineDef def, std::string* err) {
    auto fail = [&](const std::string& msg) {
        if (err) *err = msg;
        return false;
    };
    if (!skeleton_) return fail("setSkeleton first");
    if (def.states.empty()) return fail("needs at least one state");

    std::vector<MachineState> states;
    states.reserve(def.states.size());
    for (const auto& s : def.states) {
        if (s.name.empty() || s.name == "*")
            return fail("state names must be non-empty and not '*'");
        for (const auto& prev : states)
            if (prev.name == s.name)
                return fail("duplicate state '" + s.name + "'");
        if (!hasBlendSpace(s.source) && !hasClip(s.source))
            return fail("state '" + s.name + "': source '" + s.source +
                        "' is not a registered clip or blend space");
        states.push_back({s.name, s.source, s.speed, s.loop});
    }
    auto stateIndex = [&](const std::string& name) -> int {
        for (size_t i = 0; i < states.size(); ++i)
            if (states[i].name == name) return (int)i;
        return -1;
    };

    std::vector<MachineTransition> transitions;
    transitions.reserve(def.transitions.size());
    for (const auto& t : def.transitions) {
        MachineTransition mt;
        mt.from = -1;
        if (t.from != "*") {
            mt.from = stateIndex(t.from);
            if (mt.from < 0)
                return fail("transition from unknown state '" + t.from + "'");
        }
        mt.to = stateIndex(t.to);
        if (mt.to < 0)
            return fail("transition to unknown state '" + t.to + "'");
        mt.fade = std::max(t.fade, 0.0f);
        mt.autoAdvance = t.autoAdvance;
        mt.syncPhase = t.syncPhase;
        transitions.push_back(mt);
    }

    int initial = def.initial.empty() ? 0 : stateIndex(def.initial);
    if (initial < 0)
        return fail("unknown initial state '" + def.initial + "'");

    machineStates_ = std::move(states);
    machineTransitions_ = std::move(transitions);
    machineCurrent_ = -1;
    // Enter the initial state immediately, without firing onStateChanged
    // (the app installs the callback around the same time; the initial
    // entry is not a transition).
    enterState(initial, 0.0f, /*syncPhase=*/false, /*fireCallback=*/false);
    return true;
}

const std::string& AnimationPlayer::currentState() const {
    static const std::string kNone;
    return machineCurrent_ >= 0 ? machineStates_[machineCurrent_].name : kNone;
}

int AnimationPlayer::findState(const std::string& name) const {
    for (size_t i = 0; i < machineStates_.size(); ++i)
        if (machineStates_[i].name == name) return (int)i;
    return -1;
}

int AnimationPlayer::findTransition(int from, int to) const {
    for (size_t i = 0; i < machineTransitions_.size(); ++i)
        if (machineTransitions_[i].from == from &&
            machineTransitions_[i].to == to && from >= 0)
            return (int)i;
    for (size_t i = 0; i < machineTransitions_.size(); ++i)
        if (machineTransitions_[i].from == -1 &&
            machineTransitions_[i].to == to)
            return (int)i;
    return -1;
}

int AnimationPlayer::findAutoTransition(int from) const {
    for (size_t i = 0; i < machineTransitions_.size(); ++i)
        if (machineTransitions_[i].autoAdvance &&
            machineTransitions_[i].from == from && from >= 0)
            return (int)i;
    for (size_t i = 0; i < machineTransitions_.size(); ++i)
        if (machineTransitions_[i].autoAdvance &&
            machineTransitions_[i].from == -1)
            return (int)i;
    return -1;
}

bool AnimationPlayer::travel(const std::string& stateName) {
    if (machineStates_.empty()) return false;
    int idx = findState(stateName);
    if (idx < 0) return false;
    if (idx == machineCurrent_) return true;   // already there: no-op

    int tr = findTransition(machineCurrent_, idx);
    float fade = 0.0f;
    bool sync = false;
    if (tr >= 0) {
        fade = machineTransitions_[tr].fade;
        sync = machineTransitions_[tr].syncPhase;
    } else {
        LOG_WARN("AnimationPlayer::travel: no transition '%s' -> '%s'; "
                 "switching directly (fade 0)",
                 machineCurrent_ >= 0
                     ? machineStates_[machineCurrent_].name.c_str()
                     : "(suspended)",
                 stateName.c_str());
    }
    enterState(idx, fade, sync, /*fireCallback=*/true);
    return true;
}

void AnimationPlayer::enterState(int idx, float fade, bool syncPhase,
                                 bool fireCallback) {
    const int fromIdx = machineCurrent_;
    const MachineState& st = machineStates_[idx];

    // Phase carry-over source: the outgoing base track, if it is a cycle
    // (a blend space, or a looping clip).
    float oldPhase = 0.0f;
    bool oldCyclic = false;
    if (base_.valid()) {
        if (base_.space) {
            oldPhase = base_.phase;
            oldCyclic = true;
        } else if (base_.clip && base_.clip->duration > 0.0f) {
            oldPhase = base_.time / base_.clip->duration;
            oldCyclic = base_.loop;
        }
    }

    PlayOptions opts;
    opts.loop = st.loop;
    opts.speed = st.speed;
    opts.fadeTime = fade;
    if (!startBase(st.source, opts)) {
        LOG_WARN("AnimationPlayer: state '%s' source '%s' vanished",
                 st.name.c_str(), st.source.c_str());
        return;
    }
    machineCurrent_ = idx;

    // syncPhase: both states must be cycles — the incoming track starts at
    // the outgoing phase so gait cycles stay foot-aligned across the switch.
    if (syncPhase && oldCyclic) {
        if (base_.space) {
            base_.updateBlendWeights();
            base_.phase = oldPhase;
            base_.time = base_.phase * base_.cachedCycleDur;
            applyPose();
        } else if (base_.clip && base_.loop && base_.clip->duration > 0.0f) {
            base_.time = oldPhase * base_.clip->duration;
            applyPose();
        }
    }

    if (fireCallback && onStateChanged_) {
        static const std::string kNone;
        onStateChanged_(fromIdx >= 0 ? machineStates_[fromIdx].name : kNone,
                        st.name);
    }
}

// ---------------------------------------------------------------------------
// Root motion
// ---------------------------------------------------------------------------

float AnimationPlayer::yawOfQuat(const float q[4]) {
    // Yaw (about +Y, Y-up model space) of the rotated forward axis:
    // fwd = q * (0,0,1).
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float fx = 2.0f * (x * z + w * y);
    const float fz = 1.0f - 2.0f * (x * x + y * y);
    if (fx * fx + fz * fz < 1e-12f) return 0.0f;   // looking straight up/down
    return std::atan2(fx, fz);
}

float AnimationPlayer::wrapPi(float a) {
    constexpr float kTwoPi = 6.28318530717958647692f;
    a = std::fmod(a, kTwoPi);
    if (a > 3.14159265358979323846f) a -= kTwoPi;
    if (a < -3.14159265358979323846f) a += kTwoPi;
    return a;
}

bool AnimationPlayer::setRootMotion(const RootMotionOptions& opts) {
    if (!opts.enabled) {
        rmEnabled_ = false;
        rmHavePrev_ = rmHaveRef_ = false;
        rmAccumT_[0] = rmAccumT_[1] = rmAccumT_[2] = 0.0f;
        rmAccumYaw_ = 0.0f;
        return true;
    }
    if (!skeleton_) return false;

    int bone = -1;
    if (!opts.boneName.empty()) {
        bone = skeleton_->findBone(opts.boneName);
    } else if (opts.bone >= 0) {
        bone = opts.bone < (int)skeleton_->bones.size() ? opts.bone : -1;
    } else {
        // Auto-detect: the first parentless bone in index order, else bone 0.
        for (size_t i = 0; i < skeleton_->bones.size(); ++i)
            if (skeleton_->bones[i].parent < 0) { bone = (int)i; break; }
        if (bone < 0 && !skeleton_->bones.empty()) bone = 0;
    }
    if (bone < 0) return false;

    rmBone_ = bone;
    rmExtractY_ = opts.extractY;
    rmEnabled_ = true;
    rmHavePrev_ = rmHaveRef_ = false;
    rmAccumT_[0] = rmAccumT_[1] = rmAccumT_[2] = 0.0f;
    rmAccumYaw_ = 0.0f;

    // Net-loop displacements depend on the root bone: recompute for the
    // live tracks (spaces cache per entry; the flags were cleared here).
    for (auto& [name, sp] : spaces_)
        for (auto& e : sp->entries) e.netValid = false;
    base_.rootNetValid = false;
    fadeFrom_.rootNetValid = false;
    computeTrackRootNet(base_);
    computeTrackRootNet(fadeFrom_);

    // Establish the pin reference and delta baseline from the current pose
    // so the next tick never spikes (its delta covers just that tick).
    if (active_) applyPose();
    return true;
}

AnimationPlayer::RootMotionDelta AnimationPlayer::consumeRootMotion() {
    RootMotionDelta d;
    d.translation[0] = rmAccumT_[0];
    d.translation[1] = rmAccumT_[1];
    d.translation[2] = rmAccumT_[2];
    d.yaw = rmAccumYaw_;
    rmAccumT_[0] = rmAccumT_[1] = rmAccumT_[2] = 0.0f;
    rmAccumYaw_ = 0.0f;
    return d;
}

void AnimationPlayer::computeClipRootNet(const bromesh::Animation& clip,
                                         float outT[3], float* outYaw) {
    outT[0] = outT[1] = outT[2] = 0.0f;
    *outYaw = 0.0f;
    if (!skeleton_ || rmBone_ < 0) return;
    if (rmScratch_.boneCount() != bindPose_.boneCount()) rmScratch_ = bindPose_;
    const size_t off = (size_t)rmBone_ * 10;
    if (rmScratch_.data.size() < off + 10) return;

    bromesh::evaluateAnimationInto(*skeleton_, clip, 0.0f, false, rmScratch_);
    const float t0[3] = {rmScratch_.data[off], rmScratch_.data[off + 1],
                         rmScratch_.data[off + 2]};
    const float yaw0 = yawOfQuat(&rmScratch_.data[off + 3]);

    bromesh::evaluateAnimationInto(*skeleton_, clip, clip.duration, false,
                                   rmScratch_);
    outT[0] = rmScratch_.data[off] - t0[0];
    outT[1] = rmScratch_.data[off + 1] - t0[1];
    outT[2] = rmScratch_.data[off + 2] - t0[2];
    *outYaw = wrapPi(yawOfQuat(&rmScratch_.data[off + 3]) - yaw0);
}

void AnimationPlayer::computeTrackRootNet(Track& t) {
    if (!rmEnabled_ || rmBone_ < 0) return;
    if (t.space) {
        for (auto& e : t.space->entries) {
            if (e.netValid || !e.clip) continue;
            computeClipRootNet(*e.clip, e.netT, &e.netYaw);
            e.netValid = true;
        }
    } else if (t.clip && !t.rootNetValid) {
        computeClipRootNet(*t.clip, t.rootNetT, &t.rootNetYaw);
        t.rootNetValid = true;
    }
}

void AnimationPlayer::addWrapCorrection(const Track& t, float w, float* dx,
                                        float* dy, float* dz,
                                        float* dyaw) const {
    if (w <= 0.0f || t.wrapCount == 0) return;
    const float k = w * (float)t.wrapCount;
    if (t.space) {
        // The shared phase wrapped: every participating clip jumped back by
        // its own net-loop displacement, weighted by its blend weight.
        for (int i = 0; i < t.activeCount; ++i) {
            const auto& e = t.space->entries[t.activeIdx[i]];
            if (!e.netValid) continue;
            const float kw = k * t.activeW[i];
            *dx += kw * e.netT[0];
            *dy += kw * e.netT[1];
            *dz += kw * e.netT[2];
            *dyaw += kw * e.netYaw;
        }
    } else if (t.rootNetValid) {
        *dx += k * t.rootNetT[0];
        *dy += k * t.rootNetT[1];
        *dz += k * t.rootNetT[2];
        *dyaw += k * t.rootNetYaw;
    }
}

void AnimationPlayer::extractRootMotion() {
    if (rmBone_ < 0) return;
    const size_t off = (size_t)rmBone_ * 10;
    if (result_.data.size() < off + 10) return;
    float* d = result_.data.data() + off;

    const float rawT[3] = {d[0], d[1], d[2]};
    const float q[4] = {d[3], d[4], d[5], d[6]};
    const float rawYaw = yawOfQuat(q);

    if (!rmHaveRef_) {
        // Pin target: wherever the root is when extraction first sees it
        // (enable time) — no snap, and the character holds this spot.
        rmRefT_[0] = rawT[0]; rmRefT_[1] = rawT[1]; rmRefT_[2] = rawT[2];
        rmRefYaw_ = rawYaw;
        rmHaveRef_ = true;
    }

    if (rmInTick_ && rmHavePrev_) {
        float dx = rawT[0] - rmPrevT_[0];
        float dy = rawT[1] - rmPrevT_[1];
        float dz = rawT[2] - rmPrevT_[2];
        float dyaw = wrapPi(rawYaw - rmPrevYaw_);

        // Loop-wrap correction, weighted exactly like applyPose() weighted
        // the tracks into result_ (crossfade alpha, else base weight).
        float wBase, wFrom;
        if (fading_ && fadeFrom_.valid()) {
            wBase = fadeTime_ > 0.0f
                ? std::min(fadeElapsed_ / fadeTime_, 1.0f) : 1.0f;
            wFrom = 1.0f - wBase;
        } else {
            wBase = std::min(base_.weight, 1.0f);
            wFrom = 0.0f;
        }
        addWrapCorrection(base_, wBase, &dx, &dy, &dz, &dyaw);
        addWrapCorrection(fadeFrom_, wFrom, &dx, &dy, &dz, &dyaw);

        rmAccumT_[0] += dx;
        rmAccumT_[2] += dz;
        if (rmExtractY_) rmAccumT_[1] += dy;
        rmAccumYaw_ += dyaw;
    }
    // Rebase the delta baseline. Non-tick re-poses (play, seek, setBlendPos)
    // land here with rmInTick_ false: the pose change is treated like a seek
    // — no delta, next tick measures from the new pose. First tick after
    // enable/play/seek therefore never spikes.
    rmPrevT_[0] = rawT[0]; rmPrevT_[1] = rawT[1]; rmPrevT_[2] = rawT[2];
    rmPrevYaw_ = rawYaw;
    rmHavePrev_ = true;
    base_.wrapCount = 0;      // consumed (or discarded by a seek)
    fadeFrom_.wrapCount = 0;

    // Pin the pose so the character stays put: X/Z (and yaw) return to the
    // enable-time reference; Y stays authored unless extractY.
    d[0] = rmRefT_[0];
    d[2] = rmRefT_[2];
    if (rmExtractY_) d[1] = rmRefT_[1];
    const float fix = wrapPi(rmRefYaw_ - rawYaw);
    if (std::fabs(fix) > 1e-7f) {
        // q' = Ry(fix) * q — removes the extracted yaw, keeps pitch/roll.
        const float h = 0.5f * fix;
        const float s = std::sin(h), c = std::cos(h);
        d[3] = c * q[0] + s * q[2];
        d[4] = c * q[1] + s * q[3];
        d[5] = c * q[2] - s * q[0];
        d[6] = c * q[3] - s * q[1];
    }
}

// ---------------------------------------------------------------------------
// Verification seam
// ---------------------------------------------------------------------------

void AnimationPlayer::ensureWorldMatrices() {
    if (!worldMatsDirty_) return;
    if (!skeleton_) return;
    const bromesh::Pose& p =
        (result_.boneCount() == skeleton_->bones.size()) ? result_ : bindPose_;
    bromesh::computeWorldMatrices(*skeleton_, p, worldMats_);
    worldMatsDirty_ = false;
}

bool AnimationPlayer::boneWorldMatrix(int boneIndex, float out[16]) {
    if (!skeleton_) return false;
    if (boneIndex < 0 || boneIndex >= (int)skeleton_->bones.size()) return false;
    ensureWorldMatrices();
    if (worldMats_.size() < (size_t)(boneIndex + 1) * 16) return false;
    std::copy_n(worldMats_.data() + (size_t)boneIndex * 16, 16, out);
    return true;
}

bool AnimationPlayer::boneWorldMatrix(const std::string& boneName, float out[16]) {
    if (!skeleton_) return false;
    return boneWorldMatrix(skeleton_->findBone(boneName), out);
}

} // namespace bro::scene
