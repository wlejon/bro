#pragma once

/// Ending the main loop's idle wait from another thread.
///
/// A windowed bro whose frame has not changed does not present it again: it
/// waits in the OS event queue until input, the next timer, or a wake from
/// here (engine/engine_frame.cpp). Work that lands from a thread of its own
/// and needs a frame to show it (a decoded remote picture, a terminal's new
/// output, a worker's message, an agent command) calls wakeMainLoop() once
/// it has made the result visible to the main thread. Sources that are only
/// polled each frame are picked up by the wait's upper bound instead.
namespace bro::util {

/// Any thread (not a signal handler). Cheap and coalesced: a wake while one
/// is already pending costs an atomic exchange. A no-op until the engine
/// installs a waker, and wherever the loop never waits (headless, DRM).
void wakeMainLoop();

/// The engine's waker (null to remove). Called by wakeMainLoop.
void setMainLoopWaker(void (*waker)());

}  // namespace bro::util
