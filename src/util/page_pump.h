#pragma once

/// Keeping the page thread answering while it is busy.
///
/// The page thread runs the page's scripts, its frames and the window's
/// event loop. A window system calls a window that stops taking its messages
/// for a few seconds hung ("not responding" on Windows), so a long stretch of
/// work on that thread — a script's top level calling natives that compile
/// kernels, build pipelines or wait on the GPU — has to let the OS in.
///
/// The engine installs a pump (setPagePump): it lets the OS process the
/// window's messages without dispatching them to the page (they stay queued
/// for the next frame's poll), and, while the page boots, draws the page's
/// markup as it stands. It never runs page script: no timers, no rAF, no
/// input dispatch. Two things call it:
///
///   - waitPumping / runPumping: a native that blocks (a compile, a pipeline
///     build, a GPU wait) does its work on another thread, or waits in steps,
///     and pumps between them. The same code runs headless, where the pump
///     only notes that it ran; off the page thread or with no pump installed
///     the work runs inline.
///   - the script watchdog (bronze_host/script_pump.h): a stretch of script
///     longer than kPumpIntervalMs without a pump is interrupted at its next
///     loop backedge or call to pump (PumpReason::Interrupt).
///
/// Every place the page thread let the OS in notes it (notePagePump), which
/// gives the boot's longest gap between pumps, the stretch a window system
/// judges (longestPumpGap, logged beside "first frame").

#include <chrono>
#include <future>
#include <type_traits>
#include <utility>

namespace bro::util {

enum class PumpReason : unsigned char {
    Wait,       // a native waiting on CPU work (a compile): may draw
    GpuWait,    // a native waiting on the GPU: the OS's messages only (it
                // holds frame resources a frame would recycle)
    Interrupt,  // between two steps of a script: the OS's messages only
};

using PagePump = void (*)(PumpReason reason);

/// How long the page thread goes between pumps while it waits or runs script.
constexpr int kPumpIntervalMs = 16;

/// The calling thread is the page thread (the engine's, once at startup).
void setPageThread();
bool onPageThread();

/// The engine's pump; null to remove.
void setPagePump(PagePump pump);
/// Run the pump now, on the page thread (a no-op elsewhere, with none
/// installed, or from inside a pump). Notes the pump.
void pumpPageThread(PumpReason reason);
/// The page thread is inside the pump (a modal loop the OS runs from it
/// must not run a frame).
bool inPagePump();
/// Whether waits on this thread pump: the page thread with a pump installed.
bool pumpsWhileWaiting();

/// A point where the page thread let the OS process its messages.
void notePagePump();
/// Milliseconds since the last noted pump.
double msSinceLastPagePump();

/// The gap watch: from start (the window is up) to stop (the first frame).
struct PumpGap {
    double longestMs = 0.0;  // the longest stretch with no pump
    double endedAtMs = 0.0;  // when it ended, ms since the watch started
    bool running = false;
};
void startPumpWatch();
void stopPumpWatch();
PumpGap longestPumpGap();

/// Wait for `ready(timeoutMs)` (true once the awaited work is done; it may
/// block up to timeoutMs), pumping the page thread between steps.
template <class Ready>
void waitPumpingUntil(Ready&& ready, PumpReason reason = PumpReason::Wait) {
    if (!pumpsWhileWaiting()) {
        while (!ready(-1)) {
        }
        return;
    }
    while (!ready(kPumpIntervalMs)) pumpPageThread(reason);
}

template <class Future>
void waitPumpingFor(Future& f, PumpReason reason = PumpReason::Wait) {
    waitPumpingUntil(
        [&](int ms) {
            if (ms < 0) {
                f.wait();
                return true;
            }
            return f.wait_for(std::chrono::milliseconds(ms)) == std::future_status::ready;
        },
        reason);
}

/// `f.get()`, pumping the page thread while it is not ready. A caller in the
/// middle of recording GPU work passes PumpReason::GpuWait (nothing drawn).
template <class T>
T waitPumping(std::future<T>& f, PumpReason reason = PumpReason::Wait) {
    waitPumpingFor(f, reason);
    return f.get();
}
template <class T>
decltype(auto) waitPumping(const std::shared_future<T>& f, PumpReason reason = PumpReason::Wait) {
    waitPumpingFor(f, reason);
    return f.get();
}

/// `fn()`, run on another thread while the page thread pumps; inline where
/// waits do not pump. Exceptions reach the caller. `fn` must not need the
/// page thread (script, its thread-locals) to finish.
template <class Fn>
auto runPumping(Fn&& fn) -> std::invoke_result_t<Fn&> {
    if (!pumpsWhileWaiting()) return fn();
    auto f = std::async(std::launch::async, [&fn]() -> std::invoke_result_t<Fn&> { return fn(); });
    return waitPumping(f);
}

}  // namespace bro::util
