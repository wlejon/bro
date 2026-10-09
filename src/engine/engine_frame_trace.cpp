// The frame flight recorder's hooks into the frame loops (frame_trace.h), and
// starting the agent control socket (control.h). The loops time their own
// phases into frameTrace_->current(); these fill in what crosses threads —
// the layout pass the frame waited on, the raster result it showed — and
// close the record with the flip it presented on.

#include "engine/engine.h"
#include "engine/control.h"
#include "engine/frame_trace.h"
#include "engine/layout_pipeline.h"
#include "platform/window.h"
#include "render/vulkan_presenter.h"
#include "render/vulkan_swapchain.h"
#include "util/log.h"
#include "util/time.h"

#if BRO_WITH_DMABUF
#include "render/kms_direct_presenter.h"
#endif

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace bro::engine {

void Engine::traceFrameBegin(double frameStart) {
    ++frameNumber_;
    frameTrace_->begin(frameNumber_, frameStart);
    // A window system that reports when frames reached the screen (Wayland
    // presentation-time) fills in their vblanks after the fact, and is told
    // which frame the coming present belongs to.
    if (displayMode_ == DisplayMode::Windowed && window_) {
        for (const platform::PresentedFrame& p : window_->takePresentedFrames())
            if (p.presented) frameTrace_->notePresentation(p.tag, p.presentedMs, p.sequence, p.refreshMs);
        if (vulkanSwapchain_) vulkanSwapchain_->setPresentTag(frameNumber_);
    }
#if BRO_WITH_DMABUF
    if (auto* kms = vulkanPresenter_ ? vulkanPresenter_->kmsDirectPresenter() : nullptr) {
        flipCountAtFrameStart_ = kms->lastFlip().count;
        if (frameTrace_->refreshPeriodMs() <= 0.0) frameTrace_->setRefreshPeriodMs(kms->refreshPeriodMs());
    }
#endif
}

void Engine::traceLayoutSignalled() {
    FrameRecord& r = frameTrace_->current();
    r.layoutRan = true;
    layoutSignalTimeMs_ = engineNowMs_;
}

void Engine::traceLayoutClaimed() {
    FrameRecord& r = frameTrace_->current();
    r.styleMs = layoutPassTimes_.styleMs;
    r.layoutPassMs = layoutPassTimes_.layoutMs;
    r.animTickMs = layoutPassTimes_.animTickMs;
    r.activeAnimations = layoutPassTimes_.activeAnimations;
    r.promotedElements = layoutPassTimes_.promoted;
    r.layoutPerformed = layoutPassTimes_.layoutPerformed;
}

void Engine::traceRasterSignalled() {
    frameTrace_->current().rasterSignalled = true;
    contentGenSignalled_ = frameNumber_;
    contentTimeSignalled_ = layoutSignalTimeMs_;
}

void Engine::traceRasterConsumed() {
    FrameRecord& r = frameTrace_->current();
    r.rasterConsumed = true;
    r.rasterMs = rasterPassTimes_.rasterMs;
    contentGenShown_ = contentGenSignalled_;
    contentTimeShown_ = contentTimeSignalled_;
}

void Engine::traceFrameEnd() {
    FrameRecord& r = frameTrace_->current();
    r.endMs = util::currentTimeMs();
    r.contentGen = contentGenShown_;
    r.contentTimeMs = contentTimeShown_;
    // Headless has no layout thread; its step set the flag as it ticked.
    if (layoutPipeline_) r.animating = layoutPipeline_->animationsActive() || layoutPipeline_->promotedActive();
#if BRO_WITH_DMABUF
    if (auto* kms = vulkanPresenter_ ? vulkanPresenter_->kmsDirectPresenter() : nullptr) {
        const auto& flip = kms->lastFlip();
        if (flip.count != flipCountAtFrameStart_) {
            r.vblankMs = flip.vblankMs;
            r.vblankSeq = flip.sequence;
            r.presented = 1;
        }
    }
#endif
    if (!r.presented && displayMode_ != DisplayMode::Drm && r.presentMs > 0.0) r.presented = 1;
    frameTrace_->commit();
}

void Engine::startControl() {
    if (control_->running()) return;
    const char* env = std::getenv("BRO_CONTROL");
    const bool off = env && (std::strcmp(env, "0") == 0 || std::strcmp(env, "off") == 0);
    const bool on = env && *env && !off;
    if (off) return;
    if (displayMode_ != DisplayMode::Drm && !on) return;
    // The display server is "display"; anything else is named after its app
    // and pid, so several can run side by side. BRO_CONTROL=<name> (not 1)
    // picks the name.
    std::string name;
    if (on && std::strcmp(env, "1") != 0 && std::strcmp(env, "on") != 0) {
        name = env;
    } else if (displayMode_ == DisplayMode::Drm) {
        name = "display";
    } else {
        std::string app = std::filesystem::path(appDir_).filename().string();
        if (app.empty()) app = "bro";
#if defined(_WIN32)
        const long long pid = ::_getpid();
#else
        const long long pid = ::getpid();
#endif
        name = app + "-" + std::to_string(pid);
    }
    for (char& c : name)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_' && c != '.') c = '_';
    std::string why;
    if (!control_->start(name, &why)) LOG_WARN("Engine: agent control not started: %s", why.c_str());
}

}  // namespace bro::engine
