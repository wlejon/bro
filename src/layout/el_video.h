#pragma once

#include "layout/box.h"
#include "render/renderer.h"

#include <memory>
#include <string>

namespace bro::dom { class Element; }
namespace bro::video { class VideoPipeline; }

namespace bro::layout {

// Replaced-element controller for <video>. Owns a VideoPipeline and
// renders its current decoded frame into the element's content box on
// each draw. Attribute handling (src, autoplay, loop, muted, controls)
// is triggered by the JS binding via load(), play(), pause(), seek().
class ElVideo {
public:
    explicit ElVideo(render::Renderer* renderer);
    ~ElVideo();

    void draw(render::Renderer* renderer,
              dom::Element* elem,
              const htmlayout::layout::LayoutBox& box,
              float offsetX, float offsetY);

    void setElement(dom::Element* el) { elem_ = el; }
    dom::Element* element() const { return elem_; }

    // Open a WebM file; returns true on success. Does not auto-play.
    bool load(const std::string& path);

    void play();
    void pause();
    bool isPlaying() const;
    void seekTo(double seconds);
    double currentTime() const;
    double duration() const;
    bool isReady() const;   // have a decoded frame and tracks

    void getContentSize(float& w, float& h);

    int videoWidth() const { return intrinsicWidth_; }
    int videoHeight() const { return intrinsicHeight_; }

    bro::video::VideoPipeline* pipeline() const { return pipeline_.get(); }

private:
    render::Renderer* renderer_;
    dom::Element* elem_ = nullptr;
    std::unique_ptr<bro::video::VideoPipeline> pipeline_;

    int intrinsicWidth_ = 300;
    int intrinsicHeight_ = 150;
};

} // namespace bro::layout
