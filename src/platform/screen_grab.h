#pragma once

// The desktop's pixels, other apps' windows included, from the OS: what
// bro.window.captureScreen({...}) reads outside DRM (docs/screen-capture.md;
// under DRM the engine reads its own KMS scanout instead).
//
//   Windows  DXGI Desktop Duplication per output, GDI BitBlt where it fails
//   macOS    ScreenCaptureKit (needs the Screen Recording permission)
//   Linux    wlr-screencopy over $WAYLAND_DISPLAY (browl)
//
// Coordinates are the virtual desktop's, in physical pixels: on Windows the
// desktop's own (per-monitor DPI aware), on macOS and Wayland each display's
// logical origin times the largest display scale. Synchronous: call from any
// one thread at a time.

#include <cstdint>
#include <string>
#include <vector>

namespace bro::platform::screen {

struct Display {
    int index = 0;            // position in displays(); what a request names
    std::string name;
    int x = 0, y = 0;         // virtual desktop pixels
    int width = 0, height = 0;
    double scale = 1.0;       // OS content scale (pixels per logical unit)
    bool primary = false;
};

enum class Status {
    Ok,
    Unsupported,  // no backend here (no desktop session, no protocol)
    Denied,       // the OS refused: macOS Screen Recording, a secure desktop
    Failed,
};

struct Grab {
    int x = 0, y = 0;            // the grabbed rectangle, virtual desktop pixels
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;   // top-down, opaque
    std::string backend;         // "dxgi" | "gdi" | "screencapturekit" | "wlr-screencopy"
};

/// The displays, primary first, then left to right; empty where no
/// backend can see a desktop (`why` says so).
std::vector<Display> displays(std::string* why = nullptr);

/// A rectangle of the virtual desktop. width/height 0: all of it (the union
/// of the displays). The rectangle is clipped to the desktop; parts of it
/// outside every display read black. `cursor`: draw the pointer in.
Status grab(int x, int y, int width, int height, Grab& out, std::string& why, bool cursor = false);

}  // namespace bro::platform::screen
