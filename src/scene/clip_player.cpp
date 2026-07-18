#include "scene/clip_player.h"
#include "scene/scene_graph.h"
#include "scene/scene_node.h"
#include "scene/camera_node.h"
#include "scene/light_node.h"
#include "scene/mesh_node.h"
#include "scene/shape_node.h"
#include "scene/sprite_node.h"

#include <bromath/quat.h>
#include <bromath/scalar.h>
#include <bromath/vec.h>

#include <algorithm>
#include <cstring>

namespace bro::scene {

// ---------------------------------------------------------------------------
// Property access — one switch each way, mirroring tween.cpp's writes so
// dirty flags and GPU uploads happen exactly as if JS had set the property.
// ---------------------------------------------------------------------------

/// Floats per key for a property.
static int propStride(ClipProp p) {
    switch (p) {
        case ClipProp::Position:
        case ClipProp::Scale:
        case ClipProp::Color:    return 3;
        case ClipProp::Rotation: return 4;
        default:                 return 1;
    }
}

static bool clipPropApplicable(SceneNode::Type t, ClipProp p) {
    using T = SceneNode::Type;
    switch (p) {
        case ClipProp::Position:
        case ClipProp::Rotation:
        case ClipProp::Scale:     return true;
        case ClipProp::Opacity:   return t == T::Sprite || t == T::Mesh;
        case ClipProp::Color:     return t == T::Mesh || t == T::Light || t == T::Shape;
        case ClipProp::Fov:       return t == T::Camera;
        case ClipProp::Intensity:
        case ClipProp::Range:     return t == T::Light;
        case ClipProp::Emissive:
        case ClipProp::Metallic:
        case ClipProp::Roughness: return t == T::Mesh;
    }
    return false;
}

/// Read the current value of `p` on `n` into out (stride floats). Assumes
/// applicability was validated.
static void readClipProp(const SceneNode* n, ClipProp p, float* out) {
    using T = SceneNode::Type;
    switch (p) {
        case ClipProp::Position: {
            auto v = n->position(); out[0] = v.x; out[1] = v.y; out[2] = v.z;
            break;
        }
        case ClipProp::Rotation: {
            auto q = n->rotation();
            out[0] = q.x; out[1] = q.y; out[2] = q.z; out[3] = q.w;
            break;
        }
        case ClipProp::Scale: {
            auto v = n->scale(); out[0] = v.x; out[1] = v.y; out[2] = v.z;
            break;
        }
        case ClipProp::Opacity:
            if (n->type() == T::Sprite)
                out[0] = static_cast<const SpriteNode*>(n)->opacity();
            else
                out[0] = static_cast<const MeshNode*>(n)->color()[3];
            break;
        case ClipProp::Color:
            if (n->type() == T::Mesh) {
                const float* c = static_cast<const MeshNode*>(n)->color();
                out[0] = c[0]; out[1] = c[1]; out[2] = c[2];
            } else if (n->type() == T::Light) {
                auto c = static_cast<const LightNode*>(n)->color();
                out[0] = c.x; out[1] = c.y; out[2] = c.z;
            } else {
                auto c = static_cast<const ShapeNode*>(n)->fillColor();
                out[0] = c.r; out[1] = c.g; out[2] = c.b;
            }
            break;
        case ClipProp::Fov:
            out[0] = static_cast<const CameraNode*>(n)->fovY();
            break;
        case ClipProp::Intensity:
            out[0] = static_cast<const LightNode*>(n)->intensity();
            break;
        case ClipProp::Range:
            out[0] = static_cast<const LightNode*>(n)->range();
            break;
        case ClipProp::Emissive:
            out[0] = static_cast<const MeshNode*>(n)->emissive();
            break;
        case ClipProp::Metallic:
            out[0] = static_cast<const MeshNode*>(n)->metallic();
            break;
        case ClipProp::Roughness:
            out[0] = static_cast<const MeshNode*>(n)->roughness();
            break;
    }
}

/// Write `v` (stride floats) as property `p` of `n` through the same setters
/// the JS API uses. Assumes applicability was validated at play().
static void applyClipProp(SceneNode* n, ClipProp p, const float* v) {
    using T = SceneNode::Type;
    switch (p) {
        case ClipProp::Position:
            n->setPosition({v[0], v[1], v[2]});
            break;
        case ClipProp::Rotation:
            n->setRotation(bromath::qnorm({v[0], v[1], v[2], v[3]}));
            break;
        case ClipProp::Scale:
            n->setScale({v[0], v[1], v[2]});
            break;
        case ClipProp::Opacity:
            if (n->type() == T::Sprite) {
                static_cast<SpriteNode*>(n)->setOpacity(v[0]);
            } else {
                auto* m = static_cast<MeshNode*>(n);
                const float* c = m->color();
                m->setColor(c[0], c[1], c[2], v[0]);
            }
            break;
        case ClipProp::Color:
            if (n->type() == T::Mesh) {
                auto* m = static_cast<MeshNode*>(n);
                m->setColor(v[0], v[1], v[2], m->color()[3]);
            } else if (n->type() == T::Light) {
                static_cast<LightNode*>(n)->setColor(v[0], v[1], v[2]);
            } else {
                auto* sh = static_cast<ShapeNode*>(n);
                auto c = sh->fillColor();
                sh->setFillColor({v[0], v[1], v[2], c.a});
            }
            break;
        case ClipProp::Fov:
            static_cast<CameraNode*>(n)->setFovY(v[0]);
            break;
        case ClipProp::Intensity:
            static_cast<LightNode*>(n)->setIntensity(v[0]);
            break;
        case ClipProp::Range:
            static_cast<LightNode*>(n)->setRange(v[0]);
            break;
        case ClipProp::Emissive:
            static_cast<MeshNode*>(n)->setEmissive(v[0]);
            break;
        case ClipProp::Metallic:
            static_cast<MeshNode*>(n)->setMetallic(v[0]);
            break;
        case ClipProp::Roughness:
            static_cast<MeshNode*>(n)->setRoughness(v[0]);
            break;
    }
}

// ---------------------------------------------------------------------------
// Track sampling
// ---------------------------------------------------------------------------

/// Copy key `k` of `tr` into out, hemisphere-aligned against `ref` for
/// rotation tracks (ref may be null for no alignment).
static void keyValue(const AnimationClip::PropTrack& tr, size_t k,
                     const float* ref, float* out) {
    const float* src = &tr.values[k * tr.stride];
    std::memcpy(out, src, tr.stride * sizeof(float));
    if (ref && tr.prop == ClipProp::Rotation) {
        float d = out[0]*ref[0] + out[1]*ref[1] + out[2]*ref[2] + out[3]*ref[3];
        if (d < 0.0f) { out[0] = -out[0]; out[1] = -out[1]; out[2] = -out[2]; out[3] = -out[3]; }
    }
}

/// Sample a property track at time `t` (already clamped to [0, duration]).
/// `cursor` caches the key interval so sequential playback walks in O(1)
/// amortized; it is moved, never reset, so scrubbing backward also works.
static void sampleTrack(const AnimationClip::PropTrack& tr, float t,
                        uint32_t& cursor, float* out) {
    const size_t n = tr.times.size();
    const int stride = tr.stride;
    if (n == 1 || t <= tr.times.front()) {
        keyValue(tr, 0, nullptr, out);
        return;
    }
    if (t >= tr.times.back()) {
        keyValue(tr, n - 1, nullptr, out);
        return;
    }

    // Move the cached cursor to the interval containing t:
    // times[k] <= t < times[k+1].
    size_t k = cursor;
    if (k > n - 2) k = n - 2;
    while (k + 1 < n - 1 && t >= tr.times[k + 1]) k++;
    while (k > 0 && t < tr.times[k]) k--;
    cursor = (uint32_t)k;

    const float t0 = tr.times[k], t1 = tr.times[k + 1];
    float u = (t1 > t0) ? (t - t0) / (t1 - t0) : 0.0f;
    // Per-key easing warps the segment's parameter before interpolation.
    u = Tween::applyEase(tr.eases[k], u);

    switch (tr.interps[k]) {
        case ClipInterp::Step:
            keyValue(tr, k, nullptr, out);
            return;

        case ClipInterp::Linear: {
            float a[4], b[4];
            keyValue(tr, k, nullptr, a);
            keyValue(tr, k + 1, nullptr, b);
            if (tr.prop == ClipProp::Rotation) {
                // Shortest-path slerp (qslerp flips the far hemisphere).
                bromath::Quat q = bromath::qslerp(
                    {a[0], a[1], a[2], a[3]}, {b[0], b[1], b[2], b[3]}, u);
                out[0] = q.x; out[1] = q.y; out[2] = q.z; out[3] = q.w;
            } else {
                for (int i = 0; i < stride; i++)
                    out[i] = a[i] + (b[i] - a[i]) * u;
            }
            return;
        }

        case ClipInterp::Cubic: {
            // Catmull-Rom: tangents from the neighbors (one-sided at the
            // ends), Hermite basis — same polynomial the rigging clips use
            // for glTF CUBICSPLINE. Rotation keys are hemisphere-aligned to
            // key k before differencing, and the result normalized.
            float p0[4], p1[4], pPrev[4], pNext[4];
            keyValue(tr, k, nullptr, p0);
            keyValue(tr, k + 1, p0, p1);
            const bool hasPrev = k > 0;
            const bool hasNext = k + 2 < n;
            if (hasPrev) keyValue(tr, k - 1, p0, pPrev);
            if (hasNext) keyValue(tr, k + 2, p1, pNext);

            const float dt = t1 - t0;
            float m0[4], m1[4];
            for (int i = 0; i < stride; i++) {
                m0[i] = hasPrev
                    ? (p1[i] - pPrev[i]) / (tr.times[k + 1] - tr.times[k - 1])
                    : (p1[i] - p0[i]) / dt;
                m1[i] = hasNext
                    ? (pNext[i] - p0[i]) / (tr.times[k + 2] - tr.times[k])
                    : (p1[i] - p0[i]) / dt;
            }
            const float u2 = u * u, u3 = u2 * u;
            const float h00 = 2*u3 - 3*u2 + 1;
            const float h10 = u3 - 2*u2 + u;
            const float h01 = -2*u3 + 3*u2;
            const float h11 = u3 - u2;
            for (int i = 0; i < stride; i++)
                out[i] = h00*p0[i] + h10*dt*m0[i] + h01*p1[i] + h11*dt*m1[i];
            if (tr.prop == ClipProp::Rotation) {
                bromath::Quat q = bromath::qnorm({out[0], out[1], out[2], out[3]});
                out[0] = q.x; out[1] = q.y; out[2] = q.z; out[3] = q.w;
            }
            return;
        }
    }
}

/// Blend a sampled value pair: shortest-path slerp for rotations, lerp
/// otherwise. Used by the crossfade write pass.
static void mixClipValue(ClipProp p, const float* a, const float* b, float w,
                         float* out) {
    if (p == ClipProp::Rotation) {
        bromath::Quat q = bromath::qslerp(
            {a[0], a[1], a[2], a[3]}, {b[0], b[1], b[2], b[3]}, w);
        out[0] = q.x; out[1] = q.y; out[2] = q.z; out[3] = q.w;
    } else {
        int s = propStride(p);
        for (int i = 0; i < s; i++) out[i] = a[i] + (b[i] - a[i]) * w;
    }
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

void ClipPlayer::addClip(const std::string& name,
                         std::shared_ptr<const AnimationClip> clip) {
    clips_[name] = std::move(clip);
}

const AnimationClip* ClipPlayer::clip(const std::string& name) const {
    auto it = clips_.find(name);
    return it != clips_.end() ? it->second.get() : nullptr;
}

// ---------------------------------------------------------------------------
// Playback control
// ---------------------------------------------------------------------------

static const char* clipPropName(ClipProp p) {
    switch (p) {
        case ClipProp::Position:  return "position";
        case ClipProp::Rotation:  return "rotation";
        case ClipProp::Scale:     return "scale";
        case ClipProp::Opacity:   return "opacity";
        case ClipProp::Color:     return "color";
        case ClipProp::Fov:       return "fov";
        case ClipProp::Intensity: return "intensity";
        case ClipProp::Range:     return "range";
        case ClipProp::Emissive:  return "emissive";
        case ClipProp::Metallic:  return "metallic";
        case ClipProp::Roughness: return "roughness";
    }
    return "?";
}

bool ClipPlayer::play(const std::string& name, const PlayOptions& opts,
                      SceneGraph& graph, std::string& err) {
    auto it = clips_.find(name);
    if (it == clips_.end()) {
        err = "unknown clip '" + name + "' (addClip first)";
        return false;
    }
    const auto& clip = it->second;

    ActiveClip next;
    next.clip = clip;
    next.name = name;
    next.speed = opts.speed;
    const float dur = clip->duration;
    next.time = std::isnan(opts.from)
        ? (opts.speed < 0.0f ? dur : 0.0f)
        : std::clamp(opts.from, 0.0f, dur);
    next.includeLeft = true;   // an event key at exactly `from` fires

    next.tracks.reserve(clip->props.size());
    for (const auto& tr : clip->props) {
        SceneNode* n = graph.findByName(tr.target);
        if (!n) {
            err = "clip '" + name + "': no node named '" + tr.target + "'";
            return false;
        }
        if (!clipPropApplicable(n->type(), tr.prop)) {
            err = "clip '" + name + "': property '" +
                  std::string(clipPropName(tr.prop)) +
                  "' does not apply to node '" + tr.target + "'";
            return false;
        }
        ActiveTrack at;
        at.track = &tr;
        at.nodeId = n->id();
        readClipProp(n, tr.prop, at.startValue);
        next.tracks.push_back(at);
    }

    // Crossfade: the outgoing clip (even one holding its finished pose)
    // becomes the fade source. Its events are muted (advanceClip fire=false)
    // and each incoming track is matched to an outgoing track by
    // (node, property); unmatched incoming tracks blend from startValue.
    // A fade started while another fade is running drops the older source.
    if (opts.fade > 0.0f && current_.clip) {
        fadeFrom_ = std::move(current_);
        for (auto& ft : fadeFrom_.tracks) ft.shadowed = false;
        fading_ = true;
        fadeTime_ = opts.fade;
        fadeElapsed_ = 0.0f;
        for (auto& at : next.tracks) {
            for (size_t j = 0; j < fadeFrom_.tracks.size(); j++) {
                auto& ft = fadeFrom_.tracks[j];
                if (ft.nodeId == at.nodeId && ft.track->prop == at.track->prop) {
                    at.fadeFromIndex = (int)j;
                    ft.shadowed = true;
                    break;
                }
            }
        }
    } else {
        fading_ = false;
        fadeFrom_ = {};
    }

    current_ = std::move(next);
    paused_ = false;
    generation_++;
    return true;
}

void ClipPlayer::stop() {
    current_ = {};
    fadeFrom_ = {};
    fading_ = false;
    paused_ = false;
    generation_++;
}

void ClipPlayer::seek(float t, SceneGraph& graph) {
    if (!current_.clip) return;
    current_.time = std::clamp(t, 0.0f, current_.clip->duration);
    current_.includeLeft = false;   // no retro-fire, not even the key at t
    current_.finished = false;
    fading_ = false;                // scrubbing cancels a crossfade
    fadeFrom_ = {};
    generation_++;
    // Immediate evaluate + write so scrubbing works while paused.
    sampleTracks(current_);
    for (auto& at : current_.tracks) {
        if (SceneNode* n = graph.findById(at.nodeId))
            applyClipProp(n, at.track->prop, at.value);
    }
}

// ---------------------------------------------------------------------------
// Tick
// ---------------------------------------------------------------------------

bool ClipPlayer::fireEvents(ActiveClip& c, float from, float to, int dir) {
    const bool includeFrom = c.includeLeft;
    c.includeLeft = false;
    if (!onEvent_) return true;
    const uint64_t gen = generation_;
    for (const auto& track : c.clip->events) {
        if (dir > 0) {
            for (const auto& key : track.keys) {
                if (key.time > to) break;
                if (key.time < from || (key.time == from && !includeFrom))
                    continue;
                onEvent_(key.name, key.argsJson);
                if (destroyed_ || generation_ != gen) return false;
            }
        } else {
            for (size_t i = track.keys.size(); i-- > 0;) {
                const auto& key = track.keys[i];
                if (key.time < to) break;
                if (key.time > from || (key.time == from && !includeFrom))
                    continue;
                onEvent_(key.name, key.argsJson);
                if (destroyed_ || generation_ != gen) return false;
            }
        }
    }
    return true;
}

bool ClipPlayer::advanceClip(ActiveClip& c, float delta, bool fire) {
    using Loop = AnimationClip::Loop;
    const float dur = c.clip->duration;

    if (dur <= 0.0f) {
        // Degenerate zero-length clip: fire everything once, then finish
        // (Loop modes would spin forever — treat all as one-shot).
        if (fire && c.includeLeft) {
            if (!fireEvents(c, 0.0f, 0.0f, 1)) return false;
        }
        c.finished = true;
        return true;
    }

    float remaining = delta;
    int safety = 1024;   // zero-span segments can't spin: dur > 0 here
    while (remaining != 0.0f && !c.finished && safety-- > 0) {
        const int dir = (c.clip->loop == Loop::PingPong)
            ? (remaining > 0.0f ? c.pingpongDir : -c.pingpongDir)
            : (remaining > 0.0f ? 1 : -1);
        float mag = remaining > 0.0f ? remaining : -remaining;

        const float newTime = c.time + (float)dir * mag;
        const bool hitsBoundary = dir > 0 ? (newTime >= dur) : (newTime <= 0.0f);
        if (!hitsBoundary) {
            if (fire && !fireEvents(c, c.time, newTime, dir)) return false;
            c.time = newTime;
            return true;   // delta fully consumed inside the pass
        }

        const float boundary = dir > 0 ? dur : 0.0f;
        if (fire && !fireEvents(c, c.time, boundary, dir)) return false;
        mag -= dir > 0 ? (dur - c.time) : c.time;
        if (mag < 0.0f) mag = 0.0f;   // float slop when newTime grazed the edge
        remaining = (remaining > 0.0f ? 1.0f : -1.0f) * mag;
        c.time = boundary;

        switch (c.clip->loop) {
            case Loop::None:
                c.finished = true;   // tick fires onFinished after the write
                break;
            case Loop::Loop:
                c.time = dir > 0 ? 0.0f : dur;
                c.includeLeft = true;  // key at the wrapped end fires each pass
                break;
            case Loop::PingPong:
                c.pingpongDir = -c.pingpongDir;
                // The boundary key already fired inclusively on arrival; the
                // reflected pass must not re-fire it (includeLeft stays false).
                break;
        }
    }
    return true;
}

void ClipPlayer::sampleTracks(ActiveClip& c) {
    for (auto& at : c.tracks)
        sampleTrack(*at.track, c.time, at.cursor, at.value);
}

void ClipPlayer::tick(float dtSec, SceneGraph& graph) {
    if (destroyed_ || paused_ || dtSec <= 0.0f) return;
    if (!current_.clip) return;
    if (current_.finished && !fading_) return;   // holding the final pose

    const uint64_t gen = generation_;

    // Advance the crossfade source first (events muted, no finish callback).
    if (fading_) {
        fadeElapsed_ += dtSec;
        if (!fadeFrom_.finished)
            advanceClip(fadeFrom_, dtSec * fadeFrom_.speed, false);
    }

    // Advance the current clip, firing events. An event callback may
    // play/stop/seek/destroy this player — then the tick is already stale.
    const bool wasFinished = current_.finished;
    if (!wasFinished) {
        if (!advanceClip(current_, dtSec * current_.speed, true)) return;
        if (destroyed_ || generation_ != gen) return;
    }
    const bool justFinished = !wasFinished && current_.finished;

    // Sample pass (pose), then write pass — kept separate so blend weights
    // can be applied between them.
    sampleTracks(current_);
    if (fading_) {
        sampleTracks(fadeFrom_);
        const float w = fadeTime_ > 0.0f
            ? std::min(fadeElapsed_ / fadeTime_, 1.0f) : 1.0f;
        // Outgoing tracks nothing blends against keep writing until the
        // fade ends (then freeze at their last value).
        for (auto& ft : fadeFrom_.tracks) {
            if (ft.shadowed) continue;
            if (SceneNode* n = graph.findById(ft.nodeId))
                applyClipProp(n, ft.track->prop, ft.value);
        }
        float mixed[4];
        for (auto& at : current_.tracks) {
            const float* base = at.fadeFromIndex >= 0
                ? fadeFrom_.tracks[(size_t)at.fadeFromIndex].value
                : at.startValue;
            mixClipValue(at.track->prop, base, at.value, w, mixed);
            if (SceneNode* n = graph.findById(at.nodeId))
                applyClipProp(n, at.track->prop, mixed);
        }
        if (w >= 1.0f) {
            fading_ = false;
            fadeFrom_ = {};
        }
    } else {
        for (auto& at : current_.tracks) {
            if (SceneNode* n = graph.findById(at.nodeId))
                applyClipProp(n, at.track->prop, at.value);
        }
    }

    if (justFinished && onFinished_) {
        auto cb = onFinished_;   // may restart or destroy this player
        cb();
    }
}

} // namespace bro::scene
