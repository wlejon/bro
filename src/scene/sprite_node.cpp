#include "scene/sprite_node.h"
#include "scene/scene_graph.h"
#include "canvas/canvas_scene.h"

#include <stb_image.h>

#include <algorithm>

namespace bro::scene {

SpriteNode::SpriteNode(const std::string& name) : SceneNode(name) {}

SpriteNode::~SpriteNode() { releaseGL(); }

void SpriteNode::setImageData(const uint8_t* rgba, int w, int h) {
    imgW_ = w;
    imgH_ = h;
    pixels_.assign(rgba, rgba + w * h * 4);
    imageLoaded_ = true;
    textureDirty_ = true;
}

void SpriteNode::setImagePath(const std::string& path) {
    imagePath_ = path;
    imageLoaded_ = false;
    pixels_.clear();
    textureDirty_ = true;
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
                if (onEnd_) onEnd_(finished);
                if (!chain.empty()) {
                    play(chain);
                }
                return;
            }
        }
        frameIndex_ = spec.frames[animStep_];
    }
}

void SpriteNode::materializeBillboard() {
    // Lazy-load image from path, mirroring the 2D onRender path. Without
    // this, world-anchored sprites that never went through the 2D path
    // would never have pixels in time for the billboard pass.
    if (!imageLoaded_ && !imagePath_.empty()) {
        int w, h, channels;
        unsigned char* data = stbi_load(imagePath_.c_str(), &w, &h, &channels, 4);
        if (data) {
            imgW_ = w;
            imgH_ = h;
            pixels_.assign(data, data + w * h * 4);
            stbi_image_free(data);
            textureDirty_ = true;
        }
        imageLoaded_ = true;
    }
    if (pixels_.empty() || imgW_ <= 0 || imgH_ <= 0) return;
    if (texture_ != 0 && !textureDirty_) return;

    if (!texture_) glGenTextures(1, &texture_);
    glBindTexture(GL_TEXTURE_2D, texture_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, imgW_, imgH_, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, pixels_.data());
    // NEAREST works best for pixel art; the 2D canvas path uses Skia's
    // sampler with similar defaults. Switch to LINEAR when the sprite is
    // large relative to the screen quad — we don't have that info here, so
    // keep NEAREST as the cheaper, sharper default.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    texW_ = imgW_;
    texH_ = imgH_;
    textureDirty_ = false;
}

void SpriteNode::currentUvRect(float& uMin, float& vMin,
                                float& uMax, float& vMax) const {
    uMin = 0.0f; vMin = 0.0f; uMax = 1.0f; vMax = 1.0f;
    if (imgW_ <= 0 || imgH_ <= 0) return;
    float sx, sy, sw, sh;
    if (currentSheetRect(sx, sy, sw, sh)) {
        // sheet frame
    } else if (hasSourceRect_) {
        sx = srcX_; sy = srcY_; sw = srcW_; sh = srcH_;
    } else {
        return;  // full image
    }
    const float fw = static_cast<float>(imgW_);
    const float fh = static_cast<float>(imgH_);
    uMin = sx / fw;
    vMin = sy / fh;
    uMax = (sx + sw) / fw;
    vMax = (sy + sh) / fh;
}

void SpriteNode::releaseGL() {
    if (texture_) { glDeleteTextures(1, &texture_); texture_ = 0; }
    texW_ = 0;
    texH_ = 0;
}

void SpriteNode::onRender(SceneGraph& graph) {
    auto* cs = graph.canvasScene();
    if (!cs) return;

    // Lazy-load image from path
    if (!imageLoaded_ && !imagePath_.empty()) {
        int w, h, channels;
        unsigned char* data = stbi_load(imagePath_.c_str(), &w, &h, &channels, 4);
        if (data) {
            imgW_ = w;
            imgH_ = h;
            pixels_.assign(data, data + w * h * 4);
            stbi_image_free(data);
        }
        imageLoaded_ = true;
    }

    if (pixels_.empty()) return;

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
        dw = static_cast<float>(imgW_);
        dh = static_cast<float>(imgH_);
    }

    const auto& wm = worldMatrix();

    cs->save();
    cs->setTransform(wm.m[0][0], wm.m[0][1], wm.m[1][0], wm.m[1][1], wm.m[3][0], wm.m[3][1]);

    if (opacity_ < 1.0f) {
        cs->setGlobalAlpha(opacity_);
    }

    float ax = -dw * anchorX_;
    float ay = -dh * anchorY_;

    if (useSrc) {
        cs->drawImage(pixels_.data(), imgW_, imgH_,
                      sx, sy, sw, sh,
                      ax, ay, dw, dh);
    } else {
        cs->drawImage(pixels_.data(), imgW_, imgH_,
                      0, 0, static_cast<float>(imgW_), static_cast<float>(imgH_),
                      ax, ay, dw, dh);
    }

    if (opacity_ < 1.0f) {
        cs->setGlobalAlpha(1.0f);
    }

    cs->restore();
}

} // namespace bro::scene
