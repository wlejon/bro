#pragma once

#include <cstdint>

namespace bro::platform { class Window; }

namespace bro::platform::desktop {

enum class ProgressState : int32_t {
    None = 0,
    Normal = 1,
    Error = 2,
    Indeterminate = 3,
    Paused = 4
};

/// Sets the taskbar progress indicator.
bool setTaskbarProgress(const Window* window, ProgressState state, int value);

/// Headless inspection and query.
ProgressState getHeadlessProgressState();
int getHeadlessProgressValue();

} // namespace bro::platform::desktop
