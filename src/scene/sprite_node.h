#pragma once

#include "scene/scene_node.h"

#include <glad/gl.h>

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace bro::scene {

/// A renderable 2D sprite (image/texture region).
///
/// Optional spritesheet animation: configure a grid (or explicit frame list)
/// once via setSheetGrid()/setSheetFrames(), register named animations via
/// addAnimation(), then play() / stop() / set frameIndex. Frame advance runs
/// in onTick() driven by SceneGraph::tickAnimations(dt).
class SpriteNode : public SceneNode {
public:
    explicit SpriteNode(const std::string& name = "");
    ~SpriteNode() override;

    Type type() const override { return Type::Sprite; }
    void onRender(SceneGraph& graph) override;
    void onTick(float dtSec) override;

    /// Set image from RGBA pixel data. The sprite takes a copy of the data.
    void setImageData(const uint8_t* rgba, int w, int h);

    /// Set image from a file path (loaded lazily during render).
    void setImagePath(const std::string& path);
    const std::string& imagePath() const { return imagePath_; }

    /// Display size (if 0, uses image natural size — or sheet frame size if a
    /// sheet has been configured).
    void setSize(float w, float h) { width_ = w; height_ = h; }
    float width() const { return width_; }
    float height() const { return height_; }

    /// Source rectangle within the image (for manual sub-region rendering).
    /// Note: spritesheet animations override this each frame.
    void setSourceRect(float x, float y, float w, float h) {
        srcX_ = x; srcY_ = y; srcW_ = w; srcH_ = h; hasSourceRect_ = true;
    }
    bool hasSourceRect() const { return hasSourceRect_; }

    /// Anchor point (0..1). Default (0.5, 0.5) = center-origin.
    void setAnchor(float ax, float ay) { anchorX_ = ax; anchorY_ = ay; }
    float anchorX() const { return anchorX_; }
    float anchorY() const { return anchorY_; }

    /// Opacity (0..1).
    void setOpacity(float o) { opacity_ = o; }
    float opacity() const { return opacity_; }

    // Image data access (for rendering)
    const std::vector<uint8_t>& imagePixels() const { return pixels_; }
    int imageWidth() const { return imgW_; }
    int imageHeight() const { return imgH_; }
    bool hasImage() const { return !pixels_.empty(); }

    // --- Spritesheet ---

    struct Frame { float x, y, w, h; };

    /// Configure the sheet as a uniform grid. `columns`/`rows` define the grid;
    /// `frameWidth`/`frameHeight` are the per-cell size in image pixels.
    void setSheetGrid(int frameWidth, int frameHeight, int columns, int rows);

    /// Configure the sheet from an explicit list of source rects.
    void setSheetFrames(std::vector<Frame> frames);

    /// True if any sheet metadata has been set.
    bool hasSheet() const { return !frames_.empty(); }
    int frameCount() const { return static_cast<int>(frames_.size()); }
    const std::vector<Frame>& frames() const { return frames_; }

    /// Define / replace a named animation. `frameIndices` reference frames
    /// inside the sheet (0-based). `fps` is frames-per-second; 0 freezes.
    /// `next` is the animation to chain to when this one ends (loop=false);
    /// empty = stop on the last frame.
    struct AnimationSpec {
        std::vector<int> frames;
        float fps = 12.0f;
        bool loop = true;
        std::string next;
    };
    void addAnimation(const std::string& name, AnimationSpec spec);
    bool hasAnimation(const std::string& name) const {
        return animations_.find(name) != animations_.end();
    }
    const std::string& currentAnimation() const { return currentAnim_; }

    /// Start playing a registered animation by name. If no name matches and
    /// the sheet has frames, falls back to frame 0.
    void play(const std::string& name);

    /// Pause the current animation (frame index frozen at its current value).
    void stop();

    /// Resume / start the most recently played animation.
    void resume();

    bool isPlaying() const { return playing_ && !currentAnim_.empty(); }

    /// Direct frame index seek (also pauses animation playback unless one is
    /// currently active — caller can call play() again to keep cycling).
    void setFrameIndex(int idx);
    int frameIndex() const { return frameIndex_; }

    /// Per-animation-end callback: invoked once when a non-looping animation
    /// completes its last frame (just before any chained `next` animation is
    /// played). Receives the animation name.
    using AnimationEndCallback = std::function<void(const std::string&)>;
    void setOnAnimationEnd(AnimationEndCallback cb) { onEnd_ = std::move(cb); }

    // --- Internal: resolve the currently active source rect (for renderer) ---
    /// Returns true and writes [x,y,w,h] in image pixels if a sheet frame is
    /// active. Returns false if no sheet is configured (caller falls back to
    /// hasSourceRect_ / full image).
    bool currentSheetRect(float& x, float& y, float& w, float& h) const;

    /// World-anchored billboard rendering path: upload pixels_ into a GL
    /// texture if not already uploaded (or if the source image changed).
    /// No-op when there are no pixels yet (path-load happens lazily on the
    /// first 2D render pass; billboards force-load here too).
    void materializeBillboard();

    /// Compute UV sub-rect for the active sheet frame (or [0,0]-[1,1] when
    /// no sheet is configured / explicit srcRect not set).
    void currentUvRect(float& uMin, float& vMin,
                       float& uMax, float& vMax) const;

    GLuint textureId() const { return texture_; }
    int    textureWidth()  const { return texW_; }
    int    textureHeight() const { return texH_; }

    void releaseGL();

private:
    std::vector<uint8_t> pixels_;
    int imgW_ = 0, imgH_ = 0;
    std::string imagePath_;
    bool imageLoaded_ = false;

    float width_ = 0, height_ = 0;
    float srcX_ = 0, srcY_ = 0, srcW_ = 0, srcH_ = 0;
    bool hasSourceRect_ = false;

    float anchorX_ = 0.5f, anchorY_ = 0.5f;
    float opacity_ = 1.0f;

    // Sheet
    std::vector<Frame> frames_;

    // Animations
    std::unordered_map<std::string, AnimationSpec> animations_;
    std::string currentAnim_;
    int   frameIndex_ = 0;     // index into frames_ (the sheet)
    int   animStep_   = 0;     // index into currentAnim_'s frames vector
    float animElapsed_ = 0.0f; // seconds accumulated within the current frame
    bool  playing_ = false;
    AnimationEndCallback onEnd_;

    // GL texture for world-anchored billboards. Lazily created on first
    // materializeBillboard() and refreshed whenever pixels_ is replaced.
    GLuint texture_ = 0;
    int    texW_ = 0;
    int    texH_ = 0;
    bool   textureDirty_ = false;
};

} // namespace bro::scene
