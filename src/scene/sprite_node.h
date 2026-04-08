#pragma once

#include "scene/scene_node.h"
#include <cstdint>
#include <string>
#include <vector>

namespace bro::scene {

/// A renderable 2D sprite (image/texture region).
class SpriteNode : public SceneNode {
public:
    explicit SpriteNode(const std::string& name = "");

    Type type() const override { return Type::Sprite; }
    void onRender(SceneGraph& graph) override;

    /// Set image from RGBA pixel data. The sprite takes a copy of the data.
    void setImageData(const uint8_t* rgba, int w, int h);

    /// Set image from a file path (loaded lazily during render).
    void setImagePath(const std::string& path);
    const std::string& imagePath() const { return imagePath_; }

    /// Display size (if 0, uses image natural size).
    void setSize(float w, float h) { width_ = w; height_ = h; }
    float width() const { return width_; }
    float height() const { return height_; }

    /// Source rectangle within the image (for sprite sheets).
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
};

} // namespace bro::scene
