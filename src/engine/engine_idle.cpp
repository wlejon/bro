// The windowed loop with nothing to do. A frame whose picture is the one on
// screen is not presented again (holdUnchangedFrame); where the event loop
// can block (SDL), a frame is held only when nothing is about to change the
// picture, and the loop then waits for work instead of the next vblank:
// input, a wake from another thread, the next timer, a terminal's blink, or
// kIdlePollMs, whichever comes first. Anything animating (a CSS animation, a
// requestAnimationFrame loop, a playing video, a 3D scene or WebGL canvas)
// keeps every frame presented, paced by the swapchain as before.
#include "engine/engine.h"
#include "engine/control.h"
#include "engine/frame_presenter.h"
#include "engine/layout_pipeline.h"

#include "bronze_host/bronze_host.h"
#include "dom/document.h"
#include "layout/el_terminal.h"
#include "platform/event_loop.h"
#if BRO_WITH_PHYSICS
#include "physics/physics_world.h"
#endif
#include "util/main_loop_wake.h"
#include "util/time.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

namespace bro::engine {

namespace {

// The event loop util::wakeMainLoop wakes. Guarded: wakes come from decode,
// parser and worker threads, and the loop goes away at shutdown.
std::mutex g_wakeMu;
platform::EventLoop* g_wakeLoop = nullptr;

void wakeWindowLoop() {
    std::lock_guard<std::mutex> lk(g_wakeMu);
    if (g_wakeLoop) g_wakeLoop->wake();
}

}  // namespace

void Engine::installMainLoopWaker() {
    const char* env = std::getenv("BRO_IDLE_WAIT");
    idleWaitEnabled_ = !(env && std::strcmp(env, "0") == 0);
    if (!idleWaitEnabled_ || !eventLoop_ || !eventLoop_->canWaitEvents()) return;
    {
        std::lock_guard<std::mutex> lk(g_wakeMu);
        g_wakeLoop = eventLoop_.get();
    }
    util::setMainLoopWaker(&wakeWindowLoop);
}

void Engine::removeMainLoopWaker() {
    util::setMainLoopWaker(nullptr);
    std::lock_guard<std::mutex> lk(g_wakeMu);
    g_wakeLoop = nullptr;
}

bool Engine::windowIdle() const {
    if (!idleWaitEnabled_) return false;
    // Headless frames of the windowed pipeline wait on the event loop when
    // there is one, else they sleep (idleWait).
    if (!headlessPipeline_ && (!eventLoop_ || !eventLoop_->canWaitEvents())) return false;
    if (!hasRenderedOnce_ || uiDirty_ || systemDirty_ || appBaseDirty_ || splashVisible_ || pendingAppReload_)
        return false;
    if (document_ && document_->isDirty()) return false;
    // A pass in flight (or its result not yet taken), or animations it is
    // running.
    if (!layoutPipeline_ || !layoutPipeline_->isIdle() || layoutPipeline_->animationsActive() ||
        layoutPipeline_->promotedActive() || layoutPipeline_->baseAnimated())
        return false;
    if (!framePresenter_ || !framePresenter_->isRasterIdle()) return false;
    if (wheelResidualY_ != 0.0f) return false;
    if (control_ && control_->hasTickers()) return false;  // input timelines, recordings
#if BRO_WITH_PHYSICS
    if (physicsWorld_ && physicsWorld_->hasActiveBodies()) return false;
#endif
    // Script with work for the very next frame: a requestAnimationFrame
    // callback, promise jobs, a host task, a timer already due.
    if (!timePaused_ && bronze_host::hostFrameDueInMs() <= 0.0) return false;
    return true;
}

void Engine::idleWait() {
    double waitMs = kIdlePollMs;
    if (!timePaused_) {
        // Timer deadlines are on the scaled clock.
        const double scale = effectiveTimeScale();
        if (scale > 0.0) waitMs = std::min(waitMs, bronze_host::hostFrameDueInMs() / scale);
    }
    const double now = util::currentTimeMs();
    layout::ElTerminal::forEach([&](layout::ElTerminal& t) { waitMs = std::min(waitMs, t.nextRepaintInMs(now)); });
    if (waitMs <= 0.0) return;
    if (eventLoop_ && eventLoop_->canWaitEvents())
        eventLoop_->waitEvents(waitMs);
    else
        std::this_thread::sleep_for(std::chrono::microseconds(static_cast<int64_t>(waitMs * 1000.0)));
}

}  // namespace bro::engine
