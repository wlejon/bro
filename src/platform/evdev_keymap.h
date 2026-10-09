#pragma once
// Linux evdev input codes in bro's terms, on every platform: the DRM input
// path reads them from libinput, and input injected from a device bro does
// not own (a remote viewer, which speaks evdev on the wire) arrives as them
// anywhere.

#include "platform/keys.h"

#include <cstdint>

namespace bro::platform {

// evdev BTN_* codes (linux/input-event-codes.h), spelled out so this header
// needs no Linux headers.
constexpr uint32_t kEvdevBtnLeft = 0x110;
constexpr uint32_t kEvdevBtnRight = 0x111;
constexpr uint32_t kEvdevBtnMiddle = 0x112;
constexpr uint32_t kEvdevBtnSide = 0x113;
constexpr uint32_t kEvdevBtnExtra = 0x114;

/// The Scancode for an evdev KEY_* code; sc::Unknown for one with no
/// counterpart.
Scancode evdevKeyToScancode(uint32_t key);

/// The engine's mouse button number (1 left, 2 middle, 3 right, 4 and 5 the
/// side buttons) for an evdev BTN_* code; other buttons are left (1).
int evdevButtonToMouseButton(uint32_t button);

}  // namespace bro::platform
