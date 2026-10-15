#pragma once

// The interactive desktop session as an OS shell sees it, for a bro that *is*
// the session's shell over a system compositor (helm as the Winlogon shell on
// Windows): the whole screen, foreign windows included, and input injected at
// the OS's level so global hotkeys and other apps' windows see it as they see
// a real device. Under DRM bro owns both already (drm_seat / drm_input).
//
// Windows only for now; elsewhere nothing here is declared.

#include <cstdint>
#include <string>
#include <vector>

namespace bro::platform { class Window; }

namespace bro::platform::desktop {

#ifdef _WIN32

/// The window's client area on the virtual screen, physical pixels.
bool clientRectOnScreen(const Window* window, int& x, int& y, int& width, int& height);

/// RGBA of a rectangle of the composited screen (other apps' windows included).
bool captureScreenRect(int x, int y, int width, int height, std::vector<uint8_t>& rgba, std::string* why);

/// A key by its Linux evdev code (KEY_*), sent as the keyboard's scan code.
bool sendKey(uint32_t evdevCode, bool pressed);
/// The pointer to a point of the virtual screen, physical pixels.
bool sendPointerTo(int screenX, int screenY);
/// The pointer by a delta, physical pixels.
bool sendPointerBy(int dx, int dy);
/// A button by its evdev code (BTN_LEFT ... BTN_EXTRA).
bool sendButton(uint32_t evdevButton, bool pressed);
/// Wheel motion, 120 to a detent; +y toward the user (down), +x right.
bool sendWheel(int32_t wheelX, int32_t wheelY);

#endif

}  // namespace bro::platform::desktop
