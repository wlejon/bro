#pragma once

#include <algorithm>
#include <cmath>

namespace bro::engine {

/// The engine's high-DPI state. CSS px are window coordinates everywhere
/// (layout, hit testing, pointer events); only rasterization and the default
/// framebuffer are sized in device pixels.
///
///   render    device px rasterized per CSS px: the DOM layer surfaces, the
///             compositor framebuffer and the 3D scene targets are this many
///             times the CSS size. Windowed: the window's pixel density
///             (2 on a Retina Mac, 1 where windows are pixel-sized).
///   ratio     window.devicePixelRatio and the `resolution` media feature.
///             Equal to `render` on Apple and in headless; on Windows/X11 it
///             stays the OS display scale while rendering is 1:1 (see
///             Engine::updateDeviceScale).
///   drawableW/H  the default framebuffer in device px.
///
/// Headless has no backing store to follow, so both scales come from
/// `configured` (bro-headless --device-scale-factor, setDeviceScaleFactor()).
struct DeviceScale {
    float configured = 1.0f;
    float render = 1.0f;
    float ratio = 1.0f;
    int drawableW = 0;
    int drawableH = 0;

    /// A CSS extent in device px at the render scale, at least 1.
    int toDevice(int cssPx) const {
        constexpr int kMaxSide = 16384;  // one GL texture, as handleResize clamps
        return std::clamp(static_cast<int>(std::lround(cssPx * render)), 1, kMaxSide);
    }
};

} // namespace bro::engine
