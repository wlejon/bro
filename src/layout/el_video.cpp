#include "layout/el_video.h"

#include "dom/element.h"
#include "dom/event.h"
#include "js/event_dispatch.h"
#include "render/renderer.h"
#include "video/video_pipeline.h"

#include <cmath>

namespace bro::layout {

// HTMLMediaElement spec allows 4–66 Hz. 250 ms of media time is well within
// that range and matches Chromium's low-rate path.
static constexpr double kTimeUpdateIntervalSec = 0.25;


ElVideo::ElVideo(render::Renderer* renderer) : renderer_(renderer) {}
ElVideo::~ElVideo() = default;

bool ElVideo::load(const std::string& path) {
    auto p = std::make_unique<bro::video::VideoPipeline>();
    const std::string resolved = elem_ ? elem_->resolveUrl(path) : path;
    if (!p->open(resolved)) return false;
    pipeline_ = std::move(p);
    intrinsicWidth_ = pipeline_->frameWidth() > 0 ? pipeline_->frameWidth() : intrinsicWidth_;
    intrinsicHeight_ = pipeline_->frameHeight() > 0 ? pipeline_->frameHeight() : intrinsicHeight_;
    // Prime the first frame so layout has something to show before play().
    pipeline_->advanceTo(0);
    pendingLoadedMetadata_ = true;
    endedFired_ = false;
    lastTimeUpdateSec_ = -1.0;
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
    // Seek can move playback away from the end; let ended fire again if the
    // stream is re-played past its tail, and force the next timeupdate.
    endedFired_ = false;
    lastTimeUpdateSec_ = -1.0;
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
    pumpEvents();

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

void ElVideo::pumpEvents() {
    if (!jsCtx_ || !elem_ || !pipeline_) return;

    // loadedmetadata fires once after a successful open(). HTMLMediaElement
    // fires loadedmetadata even before the first frame has been decoded, but
    // at this point we've already primed one frame so dimensions/duration are
    // known — consistent with readyState >= HAVE_METADATA.
    if (pendingLoadedMetadata_) {
        pendingLoadedMetadata_ = false;
        dom::Event evt("loadedmetadata", false, false);
        evt.setIsTrusted(true);
        js::dispatchDomEvent(jsCtx_, elem_, evt);
    }

    const double t = currentTime();
    const double dur = duration();

    // timeupdate: throttle to kTimeUpdateIntervalSec of media time. Fires
    // while playing OR after a seek (seekTo resets lastTimeUpdateSec_).
    if (pipeline_->isPlaying() || lastTimeUpdateSec_ < 0.0) {
        if (lastTimeUpdateSec_ < 0.0 ||
            std::fabs(t - lastTimeUpdateSec_) >= kTimeUpdateIntervalSec) {
            lastTimeUpdateSec_ = t;
            dom::Event evt("timeupdate", false, false);
            evt.setIsTrusted(true);
            js::dispatchDomEvent(jsCtx_, elem_, evt);
        }
    }

    // ended: fire once when the demuxer has drained and we've decoded the
    // last frame. Gate on the pipeline's own EOS flag rather than comparing
    // t to duration — the last packet's pts typically falls short of the
    // container-reported duration by one frame's worth of time.
    (void)dur;
    if (!endedFired_ && pipeline_->isEnded() && pipeline_->hasFrame()) {
        endedFired_ = true;
        pipeline_->pause();
        dom::Event evt("ended", false, false);
        evt.setIsTrusted(true);
        js::dispatchDomEvent(jsCtx_, elem_, evt);
    }
}

} // namespace bro::layout
