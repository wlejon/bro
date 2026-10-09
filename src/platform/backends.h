#pragma once
// The window systems compiled into bro_platform. Internal to src/platform:
// everything else selects one with selectWindowSystem().

#include "platform/window_system.h"

namespace bro::platform {

/// SDL3 (sdl/): desktop windows on every OS.
WindowSystem& sdlWindowSystem();

/// DRM (drm_window_system.cpp): no desktop windows; bro presents through KMS
/// and reads libinput itself. Services that need no window come from SDL
/// used as a library, never its video subsystem.
WindowSystem& drmWindowSystem();

/// Wayland (wayland/): a Wayland client of its own through browl. Null when
/// the build has no Wayland backend or the compositor cannot be used (`why`
/// says why). The connection is made on the first call.
WindowSystem* waylandWindowSystem(std::string* why = nullptr);

}  // namespace bro::platform
