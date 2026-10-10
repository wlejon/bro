#pragma once

#include <cstdint>
#include <string>

namespace bro::engine {

/// Map a keycode + modifier state (platform/keys.h) to a standard web
/// KeyboardEvent.key value.
std::string sdlKeycodeToWebKey(int32_t keycode, int mod);

/// Map a scancode (platform/keys.h) to a standard web KeyboardEvent.code value.
std::string sdlScancodeToWebCode(int32_t scancode);

/// The inverse, for scripted input: a KeyboardEvent.code name ("KeyA",
/// "MediaPlayPause"), a KeyboardEvent.key name ("Enter", "MediaTrackNext",
/// "AudioVolumeUp") or a single character, to the keycode and scancode a key
/// event for it carries on a US layout. A character's scancode is left 0 for
/// the caller to look up. False when nothing has that name.
bool webKeyToKeycode(const std::string& name, int32_t& keycode, int32_t& scancode);

} // namespace bro::engine
