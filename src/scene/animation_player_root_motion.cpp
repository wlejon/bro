#include "scene/animation_player.h"

#include <algorithm>
#include <cmath>

namespace bro::scene {

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

} // namespace bro::scene
