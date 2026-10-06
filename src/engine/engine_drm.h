#pragma once

#include <memory>

namespace bro::platform {
class DrmSeatPlatform;
class DrmInputPlatform;
}

namespace bro::engine {

struct DrmPlatformContext {
    DrmPlatformContext();
    ~DrmPlatformContext();
    std::unique_ptr<platform::DrmSeatPlatform> seat;
    std::unique_ptr<platform::DrmInputPlatform> input;
};

} // namespace bro::engine
