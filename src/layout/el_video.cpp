#include "layout/el_video.h"

#include "dom/element.h"
#include "render/renderer.h"
#include "video/video_pipeline.h"

namespace bro::layout {

ElVideo::ElVideo(render::Renderer* renderer) : renderer_(renderer) {}
ElVideo::~ElVideo() = default;

bool ElVideo::load(const std::string& path) {
    auto p = std::make_unique<bro::video::VideoPipeline>();
    if (!p->open(path)) return false;
    pipeline_ = std::move(p);
    intrinsicWidth_ = pipeline_->frameWidth() > 0 ? pipeline_->frameWidth() : intrinsicWidth_;
    intrinsicHeight_ = pipeline_->frameHeight() > 0 ? pipeline_->frameHeight() : intrinsicHeight_;
    // Prime the first frame so layout has something to show before play().
    pipeline_->advance();
    return true;
}

void ElVideo::play() { if (pipeline_) pipeline_->play(); }
void ElVideo::pause() { if (pipeline_) pipeline_->pause(); }
bool ElVideo::isPlaying() const { return pipeline_ && pipeline_->isPlaying(); }

void ElVideo::seekTo(double seconds) {
    if (!pipeline_) return;
    auto ns = static_cast<bro::video::TimeNs>(seconds * 1e9);
    pipeline_->seekTo(ns);
    pipeline_->advanceTo(ns);
}

double ElVideo::currentTime() const {
    return pipeline_ ? pipeline_->currentPts() / 1e9 : 0.0;
}

double ElVideo::duration() const {
    return pipeline_ ? pipeline_->durationNs() / 1e9 : 0.0;
}

bool ElVideo::isReady() const {
    return pipeline_ && pipeline_->hasFrame();
}

void ElVideo::getContentSize(float& w, float& h) {
    w = static_cast<float>(intrinsicWidth_);
    h = static_cast<float>(intrinsicHeight_);
}

void ElVideo::draw(render::Renderer* renderer,
                   dom::Element* elem,
                   const htmlayout::layout::LayoutBox& box,
                   float offsetX, float offsetY) {
    if (!renderer || !elem) return;

    const float x = box.contentRect.x + offsetX;
    const float y = box.contentRect.y + offsetY;
    const float w = box.contentRect.width;
    const float h = box.contentRect.height;

    if (!pipeline_) {
        renderer->fillRect(x, y, w, h, render::Color{0, 0, 0, 255});
        return;
    }

    // Pull frames up to the current clock time. Paused state is tracked
    // by the pipeline's FileClock, which freezes nowNs() between
    // pause() and play().
    pipeline_->advance();

    if (pipeline_->hasFrame() && !pipeline_->currentRgba().empty()) {
        renderer->drawPixelsRGBA(pipeline_->currentRgba().data(),
                                  pipeline_->frameWidth(),
                                  pipeline_->frameHeight(),
                                  pipeline_->frameWidth() * 4,
                                  x, y, w, h);
    } else {
        renderer->fillRect(x, y, w, h, render::Color{0, 0, 0, 255});
    }
}

} // namespace bro::layout
