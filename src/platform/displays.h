#pragma once
// The attached displays, as the active WindowSystem sees them.

#include "platform/window.h"

#include <cstdint>
#include <vector>

namespace bro::platform {

class Displays {
public:
    virtual ~Displays() = default;

    /// Every attached display; `isCurrent` marks the one `window` sits on
    /// (none when `window` is null). Never empty on a desktop with a working
    /// video driver; empty where the backend has no desktop (DRM).
    virtual std::vector<DisplayInfo> list(const Window* window = nullptr) = 0;

    /// The fullscreen modes `displayId` offers; empty for an unknown id.
    virtual std::vector<DisplayModeInfo> fullscreenModes(uint32_t displayId) = 0;
};

/// The active WindowSystem's displays.
Displays& displays();

} // namespace bro::platform
