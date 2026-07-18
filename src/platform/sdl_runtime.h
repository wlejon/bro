#pragma once

namespace bro::platform {

/// Refcounted ownership of SDL library init (video + best-effort gamepad +
/// process-global hints). Historically SDL_Init lived in the Window
/// constructor and SDL_Quit in its destructor — correct while the process had
/// exactly one window, fatal with several (destroying any window would tear
/// SDL down under the rest). Every Window now acquires the runtime on
/// construction and releases it on destruction; SDL_Init runs on the 0→1
/// edge, SDL_Quit on the 1→0 edge, so the single-window lifecycle is
/// byte-for-byte what it was and multi-window just works.
///
/// Main-thread only (SDL window/video calls are main-thread anyway), so the
/// refcount is a plain int — no atomics needed.
class SdlRuntime {
public:
    /// Increment the refcount, initializing SDL on the 0→1 edge.
    /// Returns false (refcount unchanged) if SDL_Init fails.
    static bool acquire();

    /// Decrement the refcount, calling SDL_Quit on the 1→0 edge.
    static void release();
};

} // namespace bro::platform
