// Frames drawn while a page script compiles on another thread
// (bronze_host/eval_jit.cpp, compileWithPumping). What the window shows is the
// page's own static markup — its HTML and CSS are parsed and laid out before
// any script compiles — with the compile's progress published on <html>, so an
// app's loading screen is ordinary markup and CSS (docs/compile-progress.md).
// No script of the page runs from here: no timers, no rAF, no input dispatch
// beyond what the event loop's handlers already do.

#include "engine/engine.h"
#include "engine/layout_pipeline.h"

#include "dom/document.h"
#include "dom/element.h"
#include "platform/event_loop.h"
#include "platform/sdl_window.h"
#include "render/recording_renderer.h"
#include "render/skia_backend.h"
#include "util/time.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>

namespace bro::engine {

void Engine::setCompileProgress(bool compiling, double progress) {
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

void Engine::pumpCompileFrame(double progress) {
    setCompileProgress(true, progress);
    if (displayMode_ != DisplayMode::Windowed || !window_ || !renderer_ || !gl_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        return;
    }
    if (splashVisible_) {
        // The splash panel covers the window; it animates as it always has.
        pumpSplashFrame(16.67);
        return;
    }
    if (eventLoop_) {
        eventLoop_->pollEvents();
    } else {
        SDL_PumpEvents();
    }
    // A resize while compiling: the run loop's handler is not installed yet
    // at boot, so the window's size is read back directly.
    int winW = 0, winH = 0;
    if (SDL_GetWindowSize(window_->getSDLWindow(), &winW, &winH) && winW > 0 && winH > 0 &&
        (winW != viewportWidth_ || winH != viewportHeight_)) {
        handleResize(winW, winH);
    }

    // The layout thread owns the document while it runs; a reload compiles
    // with it idle, and at boot it has not started.
    if (layoutPipeline_ && !layoutPipeline_->isIdle()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        return;
    }
    auto* skia = dynamic_cast<render::SkiaRenderer*>(renderer_.get());
    if (!skia || !document_ || !document_->documentElement()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        return;
    }
    const double frameStart = util::currentTimeMs();
    if (document_->isDirty() || !document_->layoutRoot()) {
        document_->resolveStyles();
        if (document_->isLayoutDirty() || document_->isStructureDirty() || !document_->layoutRoot()) {
            document_->performLayout(static_cast<float>(viewportWidth_), static_cast<float>(contentHeight()),
                                     *textMetrics_);
            updateDocumentHeight();
        }
        document_->clearDirty();
        document_->markPaintDirty();
    }

    const int w = viewportWidth_;
    const int h = viewportHeight_;
    const int insetTop = contentTop();
    const int cw = std::max(1, w - contentRight());
    const int ch = std::max(1, h - insetTop - contentBottom());
    std::vector<UILayer> appLayers, systemLayers;
    render::CommandBuffer appCmds, sysCmds;
    recordAppLayers(appCmds, w, h, insetTop, contentRight(), contentBottom(), scrollY_);
    recordSystemPanelLayers(sysCmds, w, h);

    skia->beginFrame(w, h);
    skia->setDeviceScale(deviceScale_.render);
    replayAppLayers(skia, appCmds, screenshotHtmlPool_, screenshotHtmlPoolW_, screenshotHtmlPoolH_,
                    deviceScale_.toDevice(cw), deviceScale_.toDevice(ch), appLayers);
    replaySystemPanelLayers(skia, sysCmds, screenshotSystemPool_, screenshotSystemPoolW_, screenshotSystemPoolH_,
                            deviceScale_.toDevice(w), deviceScale_.toDevice(h), systemLayers);
    skia->setDeviceScale(1.0f);
    skia->endFrame();

    compositeLayers(appLayers, 0, insetTop, cw, ch);
    compositeLayers(systemLayers);
    window_->swapWindow();

    // About one frame per display refresh; the compile is on another core.
    const double spent = util::currentTimeMs() - frameStart;
    if (spent < 16.0) {
        std::this_thread::sleep_for(std::chrono::microseconds(static_cast<int64_t>((16.0 - spent) * 1000.0)));
    }
}

} // namespace bro::engine
