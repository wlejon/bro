#include "scene/animation_player.h"
#include "scene/skinned_mesh_node.h"

#include <cmath>

namespace bro::scene {

bool AnimationPlayer::Track::advance(float dt) {
    if (!clip || !playing) return false;
    time += dt * speed;
    float dur = clip->duration;
    if (dur <= 0.0f) { time = 0.0f; return false; }
    if (loop) {
        time = std::fmod(time, dur);
        if (time < 0.0f) time += dur;
        return false;
    }
    // Non-looping: clamp and finish once past either end (negative speed
    // finishes at 0).
    if (time >= dur) { time = dur; playing = false; return true; }
    if (time <= 0.0f && speed < 0.0f) { time = 0.0f; playing = false; return true; }
    return false;
}

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

bool AnimationPlayer::play(const std::string& name, const PlayOptions& opts) {
    if (!skeleton_) return false;
    auto it = clips_.find(name);
    if (it == clips_.end()) return false;

    if (!opts.mask.empty()) {
        // Layer slot: replaces atomically, no crossfade.
        layer_.clip = it->second;
        layer_.name = name;
        layer_.time = 0.0f;
        layer_.speed = opts.speed;
        layer_.weight = opts.weight;
        layer_.loop = opts.loop;
        layer_.playing = true;
        layerMask_ = opts.mask;
        layerMask_.resize(skeleton_->bones.size(), 0);
        layerActive_ = true;
    } else {
        if (active_ && base_.clip && opts.fadeTime > 0.0f) {
            fadeFrom_ = std::move(base_);
            fading_ = true;
            fadeTime_ = opts.fadeTime;
            fadeElapsed_ = 0.0f;
        } else {
            fading_ = false;
            fadeFrom_.reset();
        }
        base_.clip = it->second;
        base_.name = name;
        base_.time = 0.0f;
        base_.speed = opts.speed;
        base_.weight = opts.weight;
        base_.loop = opts.loop;
        base_.playing = true;
    }

    stopping_ = false;
    active_ = true;
    paused_ = false;
    applyPose();
    return true;
}

void AnimationPlayer::stop(float fadeTime) {
    if (!active_) return;
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
    layer_.reset();
    fading_ = false;
    layerActive_ = false;
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

void AnimationPlayer::setTime(float t) {
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

void AnimationPlayer::tick(float dtSec) {
    if (!active_ || paused_ || !skeleton_) return;

    // Advance clocks, collecting one-shot completions to fire after the
    // palette is staged (a callback may immediately play() something else).
    std::vector<std::string> finished;
    if (base_.advance(dtSec)) finished.push_back(base_.name);
    if (fading_) {
        fadeFrom_.advance(dtSec); // fade source finishing is not an event
        fadeElapsed_ += dtSec;
        if (fadeElapsed_ >= fadeTime_) {
            fading_ = false;
            fadeFrom_.reset();
        }
    }
    if (layerActive_ && layer_.advance(dtSec)) {
        finished.push_back(layer_.name);
        layerActive_ = false;  // one-shot layer expires; looping layers persist
        layer_.reset();
    }
    if (stopping_) stopElapsed_ += dtSec;

    applyPose();

    if (stopping_ && stopElapsed_ >= stopFadeTime_) {
        stop(0.0f);  // lands exactly on bind pose and deactivates
    }

    if (onFinished_) {
        for (const auto& n : finished) onFinished_(n);
    }
}

void AnimationPlayer::applyPose() {
    if (!skeleton_ || !active_) return;

    // Base (with optional crossfade from the previous base track).
    if (base_.clip) {
        base_.evaluate(*skeleton_);
        if (fading_ && fadeFrom_.clip) {
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

    // Masked layer on top.
    if (layerActive_ && layer_.clip) {
        layer_.evaluate(*skeleton_);
        bromesh::blendPoses(result_, layer_.pose, layer_.weight,
                            layerMask_.data());
    }

    // Stop-fade: pull the whole result toward bind pose.
    if (stopping_ && stopFadeTime_ > 0.0f) {
        float alpha = std::min(stopElapsed_ / stopFadeTime_, 1.0f);
        bromesh::blendPoses(result_, bindPose_, alpha);
    }

    bromesh::computeSkinningMatrices(*skeleton_, result_, palette_);
    owner_.setSkinningMatrices(palette_.data(), palette_.size() / 16);
    worldMatsDirty_ = true;
}

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
