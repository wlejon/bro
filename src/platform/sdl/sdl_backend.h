#pragma once
// Pieces of the SDL window system, shared between its source files.
// Internal to src/platform.

#include "platform/dialogs.h"

#include <memory>

namespace bro::platform {

class Clipboard;
class EventLoop;
class Gamepads;
class SystemInfo;

std::unique_ptr<EventLoop> createSdlEventLoop();
Clipboard& sdlClipboard();
DialogBackend& sdlDialogs();
/// Power, theme and URL opening through SDL; none of it needs SDL's video
/// subsystem, so the DRM window system uses these too.
SystemInfo& sdlSystemInfo();
/// Gamepads through SDL's gamepad subsystem, which the first SDL window
/// initializes (SdlRuntime).
Gamepads& sdlGamepads();

}  // namespace bro::platform
