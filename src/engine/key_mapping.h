#pragma once

#include <cstdint>
#include <string>

namespace bro::engine {

/// Map a keycode + modifier state (platform/keys.h) to a standard web
/// KeyboardEvent.key value.
std::string sdlKeycodeToWebKey(int32_t keycode, int mod);

/// Map a scancode (platform/keys.h) to a standard web KeyboardEvent.code value.
std::string sdlScancodeToWebCode(int32_t scancode);

} // namespace bro::engine
