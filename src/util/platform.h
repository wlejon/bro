#pragma once

#include <SDL3/SDL_keycode.h>

#include <cmath>

namespace bro::util {

/// Returns true if `mod` contains the platform's "primary" modifier for
/// conventional shortcuts (copy/paste, select-all, etc.). On macOS this is
/// Command (⌘ = SDL_KMOD_GUI); everywhere else it is Control.
inline bool hasPrimaryMod(int mod) {
#ifdef __APPLE__
    return (mod & SDL_KMOD_GUI) != 0;
#else
    return (mod & SDL_KMOD_CTRL) != 0;
#endif
}

/// Convert a raw SDL mouse-wheel delta into pixels.
///
/// On Windows/Linux, SDL3 reports wheel deltas in "tick" units — one full
/// detent of a classic mouse wheel is ±1.0. Multiplying by `pixelsPerTick`
/// (default 48) gives conventional scroll speed.
///
/// On macOS, SDL's Cocoa backend *always* delivers pixel-granularity
/// deltas: trackpad events are `NSEvent.scrollingDeltaY × 0.1`, and
/// classic wheel events are ±1 per click (small integer NSEvent line
/// units). The same `×10` scale recovers real pixel travel for the
/// trackpad (dominant case) and gives a reasonable 10 px/click step for
/// classic mice. `pixelsPerTick` acts as an overall speed multiplier
/// relative to the default (48).
inline float wheelDeltaToPixels(float delta, float pixelsPerTick) {
#ifdef __APPLE__
    return delta * 10.0f * (pixelsPerTick / 48.0f);
#else
    return delta * pixelsPerTick;
#endif
}

/// Returns the signed wheel delta to use for "vertical scroll" on a
/// vertical-only target. Normally just `dy`, but on macOS certain trackpad
/// setups deliver vertical-swipe gestures through the X axis (captured
/// empirically: the gesture produces near-zero `wheel.y` and a full
/// accel/decel curve on `wheel.x`). Native macOS apps scroll correctly
/// under the same gesture, so we match by borrowing the dominant axis
/// when the target cannot scroll horizontally.
inline float verticalWheelDelta(float dx, float dy) {
#ifdef __APPLE__
    if (std::abs(dx) > std::abs(dy)) return dx;
#else
    (void)dx;
#endif
    return dy;
}

} // namespace bro::util
