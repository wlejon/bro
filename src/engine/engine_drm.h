#pragma once

#include <memory>

namespace bro::platform {
class DrmSeatPlatform;
class DrmInputPlatform;
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
};

} // namespace bro::engine
