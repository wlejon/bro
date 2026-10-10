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
/// Gamepads through SDL's gamepad subsystem, started by Gamepads::start.
Gamepads& sdlGamepads();

/// Start SDL's gamepad subsystem (with or without its video subsystem: a
/// window system that is not SDL's, like Wayland, uses SDL only to read
/// controllers). Idempotent; false when there is no controller backend (the
/// app sees no gamepads).
bool sdlStartGamepadsOnly();
/// Whether sdlStartGamepadsOnly has started it.
bool sdlGamepadsStarted();
/// Dispatch the gamepad events SDL has queued to `loop`'s handlers (once
/// started).
void sdlPollGamepadEvents(EventLoop& loop);

}  // namespace bro::platform
