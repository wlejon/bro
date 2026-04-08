#include "scene/sprite_node.h"
#include "scene/scene_graph.h"
#include "canvas/canvas_scene.h"

#include <stb_image.h>

namespace bro::scene {

SpriteNode::SpriteNode(const std::string& name) : SceneNode(name) {}

void SpriteNode::setImageData(const uint8_t* rgba, int w, int h) {
    imgW_ = w;
    imgH_ = h;
    pixels_.assign(rgba, rgba + w * h * 4);
    imageLoaded_ = true;
}

void SpriteNode::setImagePath(const std::string& path) {
    imagePath_ = path;
    imageLoaded_ = false;
    pixels_.clear();
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

    float dw = (width_ > 0) ? width_ : static_cast<float>(imgW_);
    float dh = (height_ > 0) ? height_ : static_cast<float>(imgH_);

    const auto& wm = worldMatrix();

    cs->save();
    cs->setTransform(wm.a, wm.c, wm.b, wm.d, wm.tx, wm.ty);

    if (opacity_ < 1.0f) {
        cs->setGlobalAlpha(opacity_);
    }

    float ax = -dw * anchorX_;
    float ay = -dh * anchorY_;

    if (hasSourceRect_) {
        cs->drawImage(pixels_.data(), imgW_, imgH_,
                      srcX_, srcY_, srcW_, srcH_,
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
