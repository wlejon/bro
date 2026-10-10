// Headless frames of the windowed frame loop (docs/headless.md, "The windowed
// pipeline"). advanceTime() runs style, layout and record inline on the
// calling thread and renders only when a script asks for pixels; a window
// runs a different machine: the layout thread with its snapshot handoff, the
// raster thread replaying into double-buffered layer pools, the presenter,
// idle holds and waits, the event pump and the source watcher. What lives
// only on that path (a leak, a race, a stall) is invisible to advanceTime.
// runPipelineFrames runs Engine::windowedFrame itself, the frame Engine::run
// loops on, presenting to the headless presenter's offscreen target.

#include "engine/engine.h"
#include "engine/app_runtime.h"
#include "engine/frame_presenter.h"
#include "engine/frame_trace.h"
#include "engine/layout_pipeline.h"

#include "audio_inference/audio_inference.h"
#if BRO_WITH_PHYSICS
#include "physics/physics_world.h"
#endif
#include "platform/event_loop.h"
#include "platform/window.h"
#include "platform/window_system.h"
#include "render/vulkan_presenter.h"
#include "util/interrupt.h"
#include "util/log.h"
#include "util/time.h"

#include <chrono>
#include <thread>

namespace bro::engine {

namespace {
// The refresh a real-time run is paced to, as a FIFO swapchain paces a window.
constexpr double kHeadlessRefreshMs = 1000.0 / 60.0;
}  // namespace

Engine::PipelineRun Engine::runPipelineFrames(int frames, double stepMs) {
    PipelineRun run;
    if (displayMode_ != DisplayMode::Headless || frames <= 0) return run;

    if (!headlessPipelineStarted_) {
        headlessPipelineStarted_ = true;
        // What a window brings up beside its frames: the event loop (over the
        // hidden window), the physics and audio-inference workers, the source
        // watcher, the layout and raster threads, the main-loop waker.
        if (!eventLoop_ && window_) {
            eventLoop_ = platform::windowSystem().createEventLoop();
            if (eventLoop_) installWindowEventHandlers();
        }
#if BRO_WITH_PHYSICS
        if (physicsWorld_) physicsWorld_->startThread();
#endif
        if (audioInference_) audioInference_->startThread();
        initAppWatcher();
        startFramePipeline();
        startControl();  // BRO_CONTROL's socket, as a window opens it
        installMainLoopWaker();
        LOG_INFO("Headless: windowed frame pipeline started (event loop: %s)", eventLoop_ ? "yes" : "no");
    }

    // A clock that last moved under advanceTime (or an earlier run) starts
    // this run from now, not from then.
    lastWallTickMs_ = 0.0;
    lastUIRenderMs_ = 0.0;
    pipelineVblankMs_ = util::currentTimeMs();
    pipelineStepMs_ = stepMs > 0.0 ? stepMs : 0.0;
    pipelineRun_ = PipelineRun{};
    headlessPipeline_ = true;
    running_ = true;

    const double t0 = util::currentTimeMs();
    int n = 0;
    while (n < frames && running_ && !bro::util::interrupted()) {
        windowedFrame();
        ++n;
    }

    // Leave the workers idle: what runs next (the script, advanceTime, a
    // capture) is on this thread, over the DOM and pools they read.
    if (layoutPipeline_ && layoutPipeline_->waitForIdle()) updateDocumentHeight();
    if (framePresenter_) {
        while (framePresenter_->isRasterBusyOrRequested()) framePresenter_->waitForRaster(100.0);
        if (framePresenter_->consumeIfReady()) traceRasterConsumed();
    }
#if BRO_WITH_PHYSICS
    if (physicsWorld_) {
        while (!physicsWorld_->isIdle())
            if (!physicsWorld_->consumeStep()) std::this_thread::yield();
    }
#endif

    headlessPipeline_ = false;
    running_ = false;
    run = pipelineRun_;
    run.frames = n;
    run.wallMs = util::currentTimeMs() - t0;
    return run;
}

bool Engine::presentHeadlessPipelineFrame(const render::PresentFrame& frame) {
    bool presented = false;
    if (vulkanPresenter_) {
        presented = vulkanPresenter_->present(frame);
        if (!presented) LOG_ERROR("Engine: presenting the headless pipeline frame failed");
        frameSkiaImages_.clear();  // submitted
    } else {
        presented = static_cast<bool>(frame.below);  // the CPU composite is the frame
    }
    if (presented) {
        noteFramePresented();
        ++pipelineRun_.presented;
    }
    if (pipelineStepMs_ <= 0.0) {
        // A FIFO swapchain blocks the present until the next vblank.
        const double now = util::currentTimeMs();
        const double next = pipelineVblankMs_ + kHeadlessRefreshMs;
        if (next > now) {
            std::this_thread::sleep_for(std::chrono::microseconds(static_cast<int64_t>((next - now) * 1000.0)));
            pipelineVblankMs_ = next;
        } else {
            pipelineVblankMs_ = now;
        }
    }
    return presented;
}

}  // namespace bro::engine
