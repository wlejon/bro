// Frames drawn while the page thread is busy at boot: while a page script
// compiles on another thread (bronze_host/eval_jit.cpp, compileWithPumping),
// and while the page's scripts wait on natives that do their work elsewhere
// (util/page_pump.h). What the window shows is the page's own markup — its
// HTML and CSS are parsed and laid out before any script compiles — with a
// compile's progress published on <html>, so an app's loading screen is
// ordinary markup and CSS (docs/compile-progress.md).
// No script of the page runs from here: no timers, no rAF, no input dispatch
// beyond what the event loop's handlers already do (a compile's frames poll
// the event loop; a busy page's pump leaves the events queued).

#include "engine/engine.h"
#include "engine/layout_pipeline.h"

#include "dom/document.h"
#include "dom/element.h"
#include "platform/event_loop.h"
#include "platform/window.h"
#include "platform/window_system.h"
#include "render/recording_renderer.h"
#include "render/skia_backend.h"
#include "util/page_pump.h"
#include "util/time.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>

namespace bro::engine {

namespace {

Engine* g_pumpEngine = nullptr;

void enginePagePump(util::PumpReason reason) {
    if (g_pumpEngine) g_pumpEngine->pumpWhileBusy(reason);
}

}  // namespace

void Engine::setCompileProgress(bool compiling, double progress) {
    compileProgress_ = compiling ? std::clamp(progress, 0.0, 1.0) : -1.0;  // the splash's line (shader_status.cpp)
    dom::Element* root = document_ ? document_->documentElement() : nullptr;
    if (!root) return;
    if (!compiling) {
        if (root->hasAttribute("bro-compiling")) {
            root->removeAttribute("bro-compiling");
            root->style().removeProperty("--bro-compile-progress");
        }
        return;
    }
    char text[32];
    std::snprintf(text, sizeof(text), "%.3f", std::clamp(progress, 0.0, 1.0));
    if (!root->hasAttribute("bro-compiling")) root->setAttribute("bro-compiling", "");
    if (root->style().getProperty("--bro-compile-progress") != text) {
        root->style().setProperty("--bro-compile-progress", text);
    }
}

// The page's markup as it stands, laid out and presented: a boot frame.
void Engine::drawBootFrame() {
    // A resize while booting: the run loop's handler is not installed yet,
    // so the window's size is read back directly.
    int winW = 0, winH = 0;
    window_->getSize(winW, winH);
    if (winW > 0 && winH > 0 &&
        (winW != viewportWidth_ || winH != viewportHeight_)) {
        handleResize(winW, winH);
    }

    // The layout thread owns the document while it runs; a reload compiles
    // with it idle, and at boot it has not started.
    if (layoutPipeline_ && !layoutPipeline_->isIdle()) return;
    auto* skia = dynamic_cast<render::SkiaRenderer*>(renderer_.get());
    if (!skia || !document_ || !document_->documentElement()) return;
    // Laid out as a script's geometry read lays it out (mid-script too): the
    // paint half of the dirty flag stays armed for the first frame, and a
    // structure change leaves the frame its iframe sync.
    if (document_->isDirty()) {
        if (document_->isStructureDirty()) iframeSyncNeeded_ = true;
        flushLayoutForRead(document_.get());
    }
    if (!document_->layoutRoot()) return;

    const int w = viewportWidth_;
    const int h = viewportHeight_;
    const int insetTop = contentTop();
    const int cw = std::max(1, w - contentRight());
    const int ch = std::max(1, h - insetTop - contentBottom());
    std::vector<UILayer> appLayers, systemLayers;
    render::CommandBuffer appCmds, sysCmds;
    recordAppLayers(appCmds, w, h, insetTop, contentRight(), contentBottom(), scrollY_);
    recordSystemPanelLayers(sysCmds, w, h);

    beginGpuFrame();
    skia->beginFrame(w, h);
    skia->setDeviceScale(deviceScale_.render);
    replayAppLayers(skia, appCmds, screenshotHtmlPool_, screenshotHtmlPoolW_, screenshotHtmlPoolH_,
                    deviceScale_.toDevice(cw), deviceScale_.toDevice(ch), appLayers);
    replaySystemPanelLayers(skia, sysCmds, screenshotSystemPool_, screenshotSystemPoolW_, screenshotSystemPoolH_,
                            deviceScale_.toDevice(w), deviceScale_.toDevice(h), systemLayers);
    skia->setDeviceScale(1.0f);
    skia->endFrame();

    beginFrameComposite();
    if (drmCtx_) drmCtx_->clientLayersComposited = false;
    compositeLayers(appLayers, insetTop);
    compositeRemainingClientWindows();
    compositeLayers(systemLayers);
    presentCurrentFrame();
    releaseClientWindowFrames();
}

void Engine::pumpCompileFrame(double progress) {
    setCompileProgress(true, progress);
    if (displayMode_ != DisplayMode::Windowed || !window_ || !renderer_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        util::notePagePump();
        return;
    }
    if (splashVisible_) {
        // The splash panel covers the window; it animates as it always has.
        pumpSplashFrame(16.67);
        util::notePagePump();
        return;
    }
    if (eventLoop_) {
        eventLoop_->pollEvents();
    } else {
        platform::pumpEvents();
    }
    util::notePagePump();
    const double frameStart = util::currentTimeMs();
    drawBootFrame();

    // About one frame per display refresh; the compile is on another core.
    const double spent = util::currentTimeMs() - frameStart;
    if (spent < 16.0) {
        std::this_thread::sleep_for(std::chrono::microseconds(static_cast<int64_t>((16.0 - spent) * 1000.0)));
    }
}

// Headless has no window to answer: the pump is only noted (pumpPageThread),
// which keeps a headless run's boot timeline comparable to a window's and its
// frames deterministic. A window processes its messages, leaving the events
// queued for the next frame's poll, and while the page boots (before the run
// loop) a native's wait on CPU work draws the page as it stands. Script
// interrupted mid-run draws nothing; nor does a GPU wait (a frame would
// recycle the frame resources it waits to read), a wait inside a frame, or
// one under the splash (whose frames run its own script).
void Engine::pumpWhileBusy(util::PumpReason reason) {
    if (displayMode_ != DisplayMode::Windowed || !window_) return;
    platform::pumpEvents();
    if (reason != util::PumpReason::Wait || running_ || splashVisible_ || !renderer_) return;
    static double s_lastDrawMs = 0.0;
    const double now = util::currentTimeMs();
    if (now - s_lastDrawMs < 16.0) return;
    s_lastDrawMs = now;
    drawBootFrame();
}

void Engine::installPagePump() {
    util::setPageThread();
    g_pumpEngine = this;
    util::setPagePump(&enginePagePump);
}

void Engine::removePagePump() {
    if (g_pumpEngine != this) return;
    util::setPagePump(nullptr);
    g_pumpEngine = nullptr;
}

} // namespace bro::engine
