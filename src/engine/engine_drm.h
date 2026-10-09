#pragma once

#include "engine/window_frames.h"

#include <cstdint>
#include <memory>
#include <set>
#include <vector>

namespace bro::platform {
class DrmSeatPlatform;
class DrmInputPlatform;
struct DrmInputEvent;
}

namespace bro::compositor {
class WaylandCompositor;
struct LeasedSurfaceFrame;
}

namespace bro::engine {

// The seat and input types are compiled only with BRO_WITH_SEAT
// (src/platform/CMakeLists.txt); without it the context is empty.
struct DrmPlatformContext {
    DrmPlatformContext();
    ~DrmPlatformContext();
#if BRO_WITH_SEAT
    std::unique_ptr<platform::DrmSeatPlatform> seat;
    std::unique_ptr<platform::DrmInputPlatform> input;
#endif
#if BRO_WITH_COMPOSITOR
    std::unique_ptr<compositor::WaylandCompositor> compositor;
    std::vector<compositor::LeasedSurfaceFrame> leasedFrames;
    // The last presented frame's leases, held until its flip lands (the
    // flip's vblank is when they reached the screen).
    std::vector<compositor::LeasedSurfaceFrame> flipLeases;
    // A direct scanout's leases once its flip has landed: its buffer is on
    // screen until the next flip lands, and goes back then.
    std::vector<compositor::LeasedSurfaceFrame> onScreenLeases;
#endif
    // The flip in flight scans a client buffer out directly.
    bool flipIsDirect = false;
    // Where each held key's press went, so its auto-repeats and its release
    // follow it (engine_drm_input.cpp). Keyed by scancode.
    std::set<uint32_t> keysToClient;
    std::set<uint32_t> keysToShell;

    // The frames the shell draws around client windows (window_frames.h),
    // synced with the compositor's stack after every poll.
    WindowFrames frames;
    // Set when a paint pass's client-window breaks composited the windows
    // this frame; otherwise (an app with no such break) they go on top.
    bool clientLayersComposited = false;
    // Where the held pointer button's press went, so its moves and its
    // release follow it: the shell document, or a client window.
    bool pressToShell = false;
    bool pressToClient = false;
    // The pointer is over a client surface (its cursor is the client's).
    bool pointerOnClient = false;
};

} // namespace bro::engine
