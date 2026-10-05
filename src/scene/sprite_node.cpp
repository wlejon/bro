#include "scene/sprite_node.h"
#include "scene/scene_graph.h"
#include "canvas/canvas_scene.h"

#include "broimage/decode.h"

#include <algorithm>

namespace bro::scene {

SpriteNode::SpriteNode(const std::string& name) : SceneNode(name) {}


void SpriteNode::setImageData(const uint8_t* rgba, int w, int h) {
    image_.set(w, h, rgba);
    imageLoaded_ = true;
}

void SpriteNode::setImagePath(const std::string& path) {
    imagePath_ = path;
    imageLoaded_ = false;
    image_.clear();
}

void SpriteNode::setSheetGrid(int frameWidth, int frameHeight, int columns, int rows) {
    frames_.clear();
    if (frameWidth <= 0 || frameHeight <= 0 || columns <= 0 || rows <= 0) return;
    frames_.reserve(static_cast<size_t>(columns) * static_cast<size_t>(rows));
    for (int row = 0; row < rows; ++row) {
        for (int col = 0; col < columns; ++col) {
            frames_.push_back({
                static_cast<float>(col * frameWidth),
                static_cast<float>(row * frameHeight),
                static_cast<float>(frameWidth),
                static_cast<float>(frameHeight)
            });
        }
    }
}

void SpriteNode::setSheetFrames(std::vector<Frame> frames) {
    frames_ = std::move(frames);
}

void SpriteNode::addAnimation(const std::string& name, AnimationSpec spec) {
    // Filter out invalid frame indices to avoid runtime surprises.
    spec.frames.erase(
        std::remove_if(spec.frames.begin(), spec.frames.end(),
                       [&](int idx) { return idx < 0 || idx >= (int)frames_.size(); }),
        spec.frames.end());
    animations_[name] = std::move(spec);
}

void SpriteNode::play(const std::string& name) {
    auto it = animations_.find(name);
    if (it == animations_.end() || it->second.frames.empty()) {
        // Unknown animation: fall back to frame 0 if a sheet exists.
        currentAnim_.clear();
        if (!frames_.empty()) frameIndex_ = 0;
        playing_ = false;
        return;
    }
    currentAnim_ = name;
    animStep_ = 0;
    animElapsed_ = 0.0f;
    frameIndex_ = it->second.frames.front();
    playing_ = true;
}

void SpriteNode::stop() {
    playing_ = false;
}

void SpriteNode::resume() {
    if (!currentAnim_.empty() && animations_.count(currentAnim_)) {
        playing_ = true;
    }
}

void SpriteNode::setFrameIndex(int idx) {
    if (frames_.empty()) {
        frameIndex_ = idx;
        return;
    }
    if (idx < 0) idx = 0;
    if (idx >= (int)frames_.size()) idx = (int)frames_.size() - 1;
    frameIndex_ = idx;
}

bool SpriteNode::currentSheetRect(float& x, float& y, float& w, float& h) const {
    if (frames_.empty()) return false;
    int idx = frameIndex_;
    if (idx < 0) idx = 0;
    if (idx >= (int)frames_.size()) idx = (int)frames_.size() - 1;
    const Frame& f = frames_[idx];
    x = f.x; y = f.y; w = f.w; h = f.h;
    return true;
}

void SpriteNode::onTick(float dtSec) {
    if (!playing_ || currentAnim_.empty() || dtSec <= 0.0f) return;
    auto it = animations_.find(currentAnim_);
    if (it == animations_.end() || it->second.frames.empty() || it->second.fps <= 0.0f) return;

    AnimationSpec& spec = it->second;
    animElapsed_ += dtSec;
    float frameDur = 1.0f / spec.fps;
    while (animElapsed_ >= frameDur) {
        animElapsed_ -= frameDur;
        ++animStep_;
        if (animStep_ >= (int)spec.frames.size()) {
            if (spec.loop) {
                animStep_ = 0;
            } else {
                // Last frame done.
                animStep_ = (int)spec.frames.size() - 1;
                frameIndex_ = spec.frames[animStep_];
                std::string finished = currentAnim_;
                std::string chain = spec.next;
                playing_ = false;
                if (onEnd_) {
                    // Invoke a COPY: the callback (JS) may destroy this node,
                    // which destroys onEnd_ — a copy keeps the executing
                    // callable (and its captured JS-function ref) alive for
                    // the duration of the call.
                    auto cb = onEnd_;
                    cb(finished);
                }
                if (!chain.empty()) {
                    play(chain);
                }
                return;
            }
        }
        frameIndex_ = spec.frames[animStep_];
    }
}

void SpriteNode::ensureImageLoaded() {
    if (imageLoaded_ || imagePath_.empty()) return;
    broimage::Image img;
    if (broimage::decode_file(imagePath_, img))
        image_.adopt(img.width, img.height, std::move(img.pixels));
    imageLoaded_ = true;
}

void SpriteNode::currentUvRect(float& uMin, float& vMin,
                                float& uMax, float& vMax) const {
    uMin = 0.0f; vMin = 0.0f; uMax = 1.0f; vMax = 1.0f;
    if (image_.empty()) return;
    float sx, sy, sw, sh;
    if (currentSheetRect(sx, sy, sw, sh)) {
        // sheet frame
    } else if (hasSourceRect_) {
        sx = srcX_; sy = srcY_; sw = srcW_; sh = srcH_;
    } else {
        return;  // full image
    }
    const float fw = static_cast<float>(image_.width);
    const float fh = static_cast<float>(image_.height);
    uMin = sx / fw;
    vMin = sy / fh;
    uMax = (sx + sw) / fw;
    vMax = (sy + sh) / fh;
}

void SpriteNode::onRender(SceneGraph& graph) {
    auto* cs = graph.canvasScene();
    if (!cs) return;

    ensureImageLoaded();
    if (image_.empty()) return;

    // Source rect resolution priority:
    //   1. Active sheet frame (if a sheet is configured)
    //   2. setSourceRect()
    //   3. Full image
    float sx = 0, sy = 0, sw = 0, sh = 0;
    bool hasSheetRect = currentSheetRect(sx, sy, sw, sh);
    bool useSrc = hasSheetRect || hasSourceRect_;
    if (!hasSheetRect && hasSourceRect_) {
        sx = srcX_; sy = srcY_; sw = srcW_; sh = srcH_;
    }

    // Display size: explicit > sheet frame size > full image.
    float dw, dh;
    if (width_ > 0 || height_ > 0) {
        dw = (width_ > 0) ? width_ : sw;
        dh = (height_ > 0) ? height_ : sh;
    } else if (hasSheetRect) {
        dw = sw; dh = sh;
    } else {
        dw = static_cast<float>(image_.width);
        dh = static_cast<float>(image_.height);
    }

    const auto& wm = worldMatrix();
    const int imgW = image_.width, imgH = image_.height;

    cs->save();
    cs->setTransform(wm.at(0, 0), wm.at(1, 0), wm.at(0, 1), wm.at(1, 1), wm.at(0, 3), wm.at(1, 3));

    if (opacity_ < 1.0f) {
        cs->setGlobalAlpha(opacity_);
    }

    float ax = -dw * anchorX_;
    float ay = -dh * anchorY_;

    if (useSrc) {
        cs->drawImage(image_.rgba.data(), imgW, imgH,
                      sx, sy, sw, sh,
                      ax, ay, dw, dh);
    } else {
        cs->drawImage(image_.rgba.data(), imgW, imgH,
                      0, 0, static_cast<float>(imgW), static_cast<float>(imgH),
                      ax, ay, dw, dh);
    }

    if (opacity_ < 1.0f) {
        cs->setGlobalAlpha(1.0f);
    }

    cs->restore();
}

} // namespace bro::scene
