#pragma once

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
#endif
    // Where each held key's press went, so its auto-repeats and its release
    // follow it (engine_drm_input.cpp). Keyed by scancode.
    std::set<uint32_t> keysToClient;
    std::set<uint32_t> keysToShell;
};

} // namespace bro::engine
