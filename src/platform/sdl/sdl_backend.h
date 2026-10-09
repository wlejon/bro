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

/// SDL's gamepad subsystem without its video subsystem, for a window system
/// that is not SDL's (Wayland): SDL then only reads controllers. Idempotent;
/// false when there is no controller backend (the app sees no gamepads).
bool sdlStartGamepadsOnly();
/// Dispatch the gamepad events SDL has queued to `loop`'s handlers (after
/// sdlStartGamepadsOnly).
void sdlPollGamepadEvents(EventLoop& loop);

}  // namespace bro::platform
